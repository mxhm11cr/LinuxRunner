#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <sys/mman.h>
#include <stdatomic.h>
static _Atomic size_t live_bytes, peak_bytes;
static int fail_map;
static void *tracked_mmap(void *a, size_t n, int p, int f, int fd, off_t o) {
    if (fail_map) { fail_map = 0; return MAP_FAILED; }
    void *map = mmap(a, n, p, f, fd, o);
    if (map != MAP_FAILED) {
        size_t live = atomic_fetch_add(&live_bytes, n) + n;
        size_t peak = atomic_load(&peak_bytes);
        while (live > peak && !atomic_compare_exchange_weak(&peak_bytes, &peak, live)) {}
    }
    return map;
}
static int tracked_munmap(void *a, size_t n) {
    atomic_fetch_sub(&live_bytes, n);
    return munmap(a, n);
}
#define mmap tracked_mmap
#define munmap tracked_munmap
#include "kernel/page_pool.h"
#undef mmap
#undef munmap

static void *stress(void *unused) {
    (void)unused;
    for (unsigned round = 0; round < 1000; round++) {
        struct guest_page_slab *owners[17];
        unsigned char *pages[17];
        for (unsigned i = 0; i < 17; i++) {
            pages[i] = guest_page_allocate(16384, 4096, &owners[i]);
            assert(pages[i] != MAP_FAILED);
            for (unsigned j = 0; j < 4096; j++) assert(pages[i][j] == 0);
            memset(pages[i], i + 1, 4096);
        }
        for (unsigned i = 0; i < 17; i++) {
            for (unsigned j = 0; j < 4096; j++) assert(pages[i][j] == i + 1);
            guest_page_release(owners[i], pages[i], 4096);
        }
    }
    return NULL;
}
int main(void) {
    enum { count = 1024 };
    struct guest_page_slab *owners[count];
    unsigned char *pages[count];
    fail_map = 1;
    assert(guest_page_allocate(16384, 4096, &owners[0]) == MAP_FAILED);
    assert(!guest_page_available && !live_bytes);
    for (unsigned i = 0; i < count; i++) {
        pages[i] = guest_page_allocate(16384, 4096, &owners[i]);
        assert(pages[i] != MAP_FAILED);
        memset(pages[i], i % 251, 4096);
    }
    assert(live_bytes == count * 4096);
    assert(peak_bytes == count * 4096);
    for (unsigned i = 0; i < count; i += 2) guest_page_release(owners[i], pages[i], 4096);
    for (unsigned i = 1; i < count; i += 2) {
        for (unsigned j = 0; j < 4096; j++) assert(pages[i][j] == i % 251);
        guest_page_release(owners[i], pages[i], 4096);
    }
    assert(!live_bytes && !guest_page_available);
    pthread_t threads[4];
    for (unsigned i = 0; i < 4; i++) assert(!pthread_create(&threads[i], NULL, stress, NULL));
    for (unsigned i = 0; i < 4; i++) assert(!pthread_join(threads[i], NULL));
    assert(!live_bytes && !guest_page_available);
    void *plain = guest_page_allocate(4096, 4096, &owners[0]);
    assert(plain != MAP_FAILED && !owners[0]);
    guest_page_release(owners[0], plain, 4096);
    assert(!live_bytes);
    puts("guest-page-pool: 4 MiB backing for 1024 guest pages; isolation, zero fill, failure and concurrency passed");
}
