#ifndef GUEST_PAGE_POOL_H
#define GUEST_PAGE_POOL_H
#include <errno.h>
#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>

// Keep small guest pages in full host pages. Only pages with free slots are listed.
struct guest_page_slab {
    void *base;
    size_t bytes, slot_bytes;
    uint64_t available, all;
    struct guest_page_slab *next, *previous;
};
static pthread_mutex_t guest_page_lock = PTHREAD_MUTEX_INITIALIZER;
static struct guest_page_slab *guest_page_available;

static void guest_page_unlink(struct guest_page_slab *slab) {
    if (slab->previous) slab->previous->next = slab->next;
    else guest_page_available = slab->next;
    if (slab->next) slab->next->previous = slab->previous;
    slab->next = slab->previous = NULL;
}
static void guest_page_link(struct guest_page_slab *slab) {
    slab->next = guest_page_available;
    slab->previous = NULL;
    if (slab->next) slab->next->previous = slab;
    guest_page_available = slab;
}
static void *guest_page_allocate(size_t host_bytes, size_t guest_bytes, struct guest_page_slab **owner) {
    *owner = NULL;
    if (host_bytes <= guest_bytes)
        return mmap(NULL, guest_bytes, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    size_t slots = host_bytes / guest_bytes;
    if (host_bytes % guest_bytes || slots > 64) { errno = EINVAL; return MAP_FAILED; }
    pthread_mutex_lock(&guest_page_lock);
    struct guest_page_slab *slab = guest_page_available;
    if (!slab) {
        slab = calloc(1, sizeof(*slab));
        if (!slab) { pthread_mutex_unlock(&guest_page_lock); return MAP_FAILED; }
        slab->base = mmap(NULL, host_bytes, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (slab->base == MAP_FAILED) { free(slab); pthread_mutex_unlock(&guest_page_lock); return MAP_FAILED; }
        slab->bytes = host_bytes;
        slab->slot_bytes = guest_bytes;
        slab->all = slots == 64 ? UINT64_MAX : (UINT64_C(1) << slots) - 1;
        slab->available = slab->all;
        guest_page_link(slab);
    }
    unsigned slot = (unsigned)__builtin_ctzll(slab->available);
    slab->available &= ~(UINT64_C(1) << slot);
    if (!slab->available) guest_page_unlink(slab);
    pthread_mutex_unlock(&guest_page_lock);
    *owner = slab;
    void *page = (char *)slab->base + slot * guest_bytes;
    memset(page, 0, guest_bytes);
    return page;
}
static void guest_page_release(struct guest_page_slab *slab, void *page, size_t bytes) {
    if (!slab) { munmap(page, bytes); return; }
    unsigned slot = (unsigned)(((char *)page - (char *)slab->base) / slab->slot_bytes);
    pthread_mutex_lock(&guest_page_lock);
    if (!slab->available) guest_page_link(slab);
    slab->available |= UINT64_C(1) << slot;
    if (slab->available == slab->all) {
        guest_page_unlink(slab);
        munmap(slab->base, slab->bytes);
        free(slab);
    }
    pthread_mutex_unlock(&guest_page_lock);
}
#endif
