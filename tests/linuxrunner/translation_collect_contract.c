#include <assert.h>
#include <pthread.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include "kernel/init.h"
#include "kernel/calls.h"
#include "kernel/memory.h"
#include "fs/real.h"
#include "asbestos/asbestos.h"
#include "emu/tlb.h"
#include "emu/interrupt.h"
extern _Atomic uint64_t asbestos_stat_blocks, asbestos_stat_code_words, asbestos_stat_retired_bytes, asbestos_stat_cap_flushes;
static struct asbestos *space;
static atomic_bool holding, release_reader, collected;
static void *reader(void *unused) {
    (void)unused;
    read_wrlock(&space->jetsam_lock);
    atomic_store(&holding, true);
    while (!atomic_load(&release_reader)) sched_yield();
    read_wrunlock(&space->jetsam_lock);
    return NULL;
}
static void *collector(void *unused) {
    (void)unused;
    asbestos_collect(space);
    atomic_store(&collected, true);
    return NULL;
}
int main(int argc, char **argv) {
    bool cap = argc > 1 && !strcmp(argv[1], "cap");
    if (cap) setenv("ISH_CODE_CAP_MB", "1", 1);
    assert(!mount_root(&realfs, "/") && !become_first_process());
    enum { count = 24000 };
    const addr_t code = 0x10000;
    assert(!pt_map_nothing(current->mem, PAGE(code), PAGE_ROUND_UP(count * 8), P_READ | P_WRITE | P_EXEC));
    uint32_t *instructions = mem_pt(current->mem, PAGE(code))->data->data;
    for (unsigned i = 0; i < count; i++) {
        instructions[i * 2] = 0xd28000e0; // mov x0, #7
        instructions[i * 2 + 1] = 0xd4200000; // brk
    }
    struct tlb *tlb = calloc(1, sizeof(*tlb)); assert(tlb);
    struct cpu_state *cpu = &current->cpu;
    read_wrlock(&current->mem->lock);
    tlb_refresh(tlb, &current->mem->mmu);
    for (unsigned i = 0; i < count; i++) {
        cpu->pc = code + i * 8;
        assert(cpu_run_to_interrupt(cpu, tlb) == INT_BREAKPOINT);
        assert(cpu->regs[0] == 7);
    }
    read_wrunlock(&current->mem->lock);
    space = current->mem->mmu.asbestos;
    if (cap) {
        assert(atomic_load(&asbestos_stat_cap_flushes) > 0);
        assert(space->mem_used * sizeof(unsigned long) + space->num_blocks * sizeof(struct fiber_block) <= (1u << 20));
        tlb_free(tlb);
        mem_destroy(current->mem);
        assert(!atomic_load(&asbestos_stat_blocks) && !atomic_load(&asbestos_stat_code_words));
        assert(!atomic_load(&asbestos_stat_retired_bytes));
        puts("translation-cap: actual execution stayed within the configured cache limit");
        return 0;
    }
    asbestos_invalidate_all(space);
    assert(space->retired_bytes >= (4u << 20));
    assert(atomic_load(&asbestos_stat_retired_bytes) == space->retired_bytes);
    pthread_t busy, reclaim;
    assert(!pthread_create(&busy, NULL, reader, NULL));
    while (!atomic_load(&holding)) sched_yield();
    assert(!pthread_create(&reclaim, NULL, collector, NULL));
    while (!atomic_load(&space->collecting)) sched_yield();
    assert(!atomic_load(&collected));
    read_wrlock(&current->mem->lock);
    cpu->pc = code;
    assert(cpu_run_to_interrupt(cpu, tlb) == INT_TIMER);
    read_wrunlock(&current->mem->lock);
    atomic_store(&release_reader, true);
    assert(!pthread_join(busy, NULL) && !pthread_join(reclaim, NULL));
    assert(atomic_load(&collected) && !space->retired_bytes);
    assert(!atomic_load(&asbestos_stat_retired_bytes));
    read_wrlock(&current->mem->lock);
    cpu->pc = code;
    assert(cpu_run_to_interrupt(cpu, tlb) == INT_BREAKPOINT);
    assert(cpu->regs[0] == 7);
    read_wrunlock(&current->mem->lock);
    tlb_free(tlb);
    assert(!pt_unmap(current->mem, PAGE(code), PAGE_ROUND_UP(count * 8)));
    asbestos_collect(space);
    mem_destroy(current->mem);
    assert(!atomic_load(&asbestos_stat_blocks));
    assert(!atomic_load(&asbestos_stat_code_words));
    assert(!atomic_load(&asbestos_stat_retired_bytes));
    puts("translation-collect: competing reader drained, new slices yielded, stale cache cleared, counters balanced");
}
