#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "kernel/init.h"
#include "kernel/calls.h"
#include "kernel/memory.h"
#include "kernel/mm.h"
#include "fs/real.h"
int main(void) {
    assert(mount_root(&realfs, "/") == 0 && become_first_process() == 0);
    const page_t start = 0x90;
    for (unsigned i = 0; i < 8; i++) {
        assert(pt_map_nothing(current->mem, start + i, 1, P_READ | P_WRITE) == 0);
        memset(mem_pt(current->mem, start + i)->data->data, i + 1, PAGE_SIZE);
    }
    struct mem child; mem_init(&child);
    assert(pt_copy_on_write(current->mem, &child, start, 8) == 0);
    for (unsigned i = 0; i < 8; i++) {
        char value = 40 + i;
        assert(!user_write((start + i) * PAGE_SIZE, &value, 1));
        struct pt_entry *old = mem_pt(&child, start + i);
        assert(*(unsigned char *)((char *)old->data->data + old->offset) == i + 1);
        assert(*(char *)mem_pt(current->mem, start + i)->data->data == value);
    }
    write_wrlock(&current->mem->lock);
    assert(!pt_set_flags(current->mem, start, 1, 0));
    write_wrunlock(&current->mem->lock);
    char value = 99;
    assert(user_write(start * PAGE_SIZE, &value, 1));
    assert(!user_write((start + 1) * PAGE_SIZE, &value, 1));
    write_wrlock(&current->mem->lock);
    assert(!pt_set_flags(current->mem, start, 1, P_READ | P_WRITE));
    write_wrunlock(&current->mem->lock);
    assert(!user_write(start * PAGE_SIZE, &value, 1));
    mem_destroy(&child);
    assert(!pt_unmap(current->mem, start, 8));
    assert(!atomic_load(&anon_page_count));
    puts("guest-page-memory: actual kernel fork, copy-on-write, permissions and release passed");
}
