#include <assert.h>
#include <stdio.h>
#include "platform/guest_memory.h"
int main(void) {
    const uint64_t mib = UINT64_C(1024) * 1024;
    assert(guest_memory_total(3072 * mib, 1536 * mib) == 768 * mib);
    assert(guest_memory_total(8192 * mib, 4096 * mib) == 2048 * mib);
    assert(guest_memory_total(1024 * mib, 4096 * mib) == 512 * mib);
    assert(guest_memory_total(3072 * mib, 0) == 1536 * mib);
    assert(guest_memory_total(0, 0) == 0);
    assert(guest_memory_total(10001, 9001) == 4096);
    puts("guest-memory-budget: process limit, physical limit and page alignment passed");
}
