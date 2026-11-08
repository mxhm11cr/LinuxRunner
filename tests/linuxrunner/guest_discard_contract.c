#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include "kernel/init.h"
#include "kernel/calls.h"
#include "kernel/memory.h"
#include "kernel/mm.h"
#include "fs/real.h"
static void check_bytes(struct mem *mem, page_t page, unsigned char expected) {
    struct pt_entry *entry = mem_pt(mem, page);
    unsigned char *data = (unsigned char *)entry->data->data + entry->offset;
    for (unsigned i = 0; i < PAGE_SIZE; i++) assert(data[i] == expected);
}
int main(void) {
    assert(!mount_root(&realfs, "/") && !become_first_process());
    const page_t start = 0x100;
    const pages_t pages = real_page_size / PAGE_SIZE * 16;
    assert(!pt_map_nothing(current->mem, start, pages, P_READ | P_WRITE));
    void *base = mem_pt(current->mem, start)->data->data;
    memset(base, 42, pages * PAGE_SIZE);
    char *residency = calloc(pages, 1);
    assert(residency && !mincore(base, pages * PAGE_SIZE, residency));
    assert(residency[0] & 1);
    assert(!sys_madvise(start * PAGE_SIZE, pages * PAGE_SIZE, 4));
    assert(!mincore(base, pages * PAGE_SIZE, residency));
    for (unsigned i = 0; i < pages * PAGE_SIZE / real_page_size; i++) assert(!(residency[i] & 1));
    for (pages_t i = 0; i < pages; i++) check_bytes(current->mem, start + i, 0);
    free(residency);
    assert(!pt_unmap(current->mem, start, pages));
    assert(!pt_map_nothing(current->mem, start, 8, P_READ | P_WRITE));
    memset(mem_pt(current->mem, start)->data->data, 73, 8 * PAGE_SIZE);
    struct mem child; mem_init(&child);
    assert(!pt_copy_on_write(current->mem, &child, start, 8));
    assert(!sys_madvise(start * PAGE_SIZE + PAGE_SIZE, 2 * PAGE_SIZE, 4));
    check_bytes(current->mem, start, 73);
    check_bytes(current->mem, start + 1, 0);
    check_bytes(current->mem, start + 2, 0);
    check_bytes(current->mem, start + 3, 73);
    for (unsigned i = 0; i < 8; i++) check_bytes(&child, start + i, 73);
    mem_destroy(&child);
    assert(!pt_unmap(current->mem, start, 8));
    assert(!pt_map_nothing(current->mem, start, 1, P_READ | P_WRITE | P_SHARED));
    memset(mem_pt(current->mem, start)->data->data, 91, PAGE_SIZE);
    assert(!sys_madvise(start * PAGE_SIZE, PAGE_SIZE, 4));
    check_bytes(current->mem, start, 91);
    assert(!pt_unmap(current->mem, start, 1));
    assert(!atomic_load(&anon_page_count));
    puts("guest-discard: dirty backing released; zero fill, partial ranges, fork and shared pages passed");
}
