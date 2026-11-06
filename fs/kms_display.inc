// Included by drm.c; deliberately independent of guest task state. The host
// never keeps a pointer into a compositor-owned buffer after flip completion.
#include "fs/kms_display.h"
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
static struct {
    pthread_mutex_t lock;
    uint64_t token, next_token, sequence;
    struct lr_kms_frame latest;
    struct lr_kms_input input[1024];
    unsigned head, count;
} display = {.lock = PTHREAD_MUTEX_INITIALIZER};
uint64_t lr_kms_display_begin(void) {
    pthread_mutex_lock(&display.lock);
    free(display.latest.pixels); display.latest = (struct lr_kms_frame){0};
    display.token = ++display.next_token; display.head = 0; display.count = 1;
    display.input[0] = (struct lr_kms_input){.type = 3};
    uint64_t token = display.token;
    pthread_mutex_unlock(&display.lock); return token;
}
void lr_kms_display_end(uint64_t token) {
    pthread_mutex_lock(&display.lock);
    if (token && token == display.token) {
        display.token = 0; display.head = display.count = 0;
        free(display.latest.pixels); display.latest = (struct lr_kms_frame){0};
    }
    pthread_mutex_unlock(&display.lock);
}
static bool lr_kms_display_active(void) {
    pthread_mutex_lock(&display.lock); bool active = display.token != 0;
    pthread_mutex_unlock(&display.lock); return active;
}
// Called with kms.lock held, before the guest is told it may reuse scanout.
static void lr_kms_display_publish(const void *pixels, uint32_t w, uint32_t h, uint32_t pitch) {
    if (!pixels || !w || !h || w > 2560 || h > 1600 || pitch < w * 4) return;
    pthread_mutex_lock(&display.lock);
    if (display.token) {
        size_t bytes = (size_t)w * h * 4;
        void *copy = malloc(bytes);
        if (copy) {
            for (unsigned y = 0; y < h; y++) {
                uint32_t *row = (uint32_t *)copy + (size_t)y * w;
                memcpy(row, (const char *)pixels + (size_t)y * pitch, w * 4);
                // XRGB has undefined alpha. Desktop scanout is always opaque.
                for (unsigned x = 0; x < w; x++) row[x] |= 0xff000000U;
            }
            free(display.latest.pixels);
            display.latest = (struct lr_kms_frame){copy, w, h, ++display.sequence};
        }
    }
    pthread_mutex_unlock(&display.lock);
}
bool lr_kms_display_take(uint64_t token, struct lr_kms_frame *frame) {
    pthread_mutex_lock(&display.lock);
    bool ready = token && token == display.token && display.latest.pixels;
    if (ready) { *frame = display.latest; display.latest = (struct lr_kms_frame){0}; }
    pthread_mutex_unlock(&display.lock); return ready;
}
bool lr_kms_display_input(uint64_t token, struct lr_kms_input e) {
    pthread_mutex_lock(&display.lock);
    bool ok = token && token == display.token && (e.type == 1 || e.type == 2);
    // Coalesce consecutive motion only; never reorder button/key transitions.
    if (ok && e.type == 1 && display.count) {
        unsigned last = (display.head + display.count - 1) % 1024;
        if (display.input[last].type == 1 && display.input[last].buttons == e.buttons) {
            display.input[last] = e; pthread_mutex_unlock(&display.lock); return true;
        }
    }
    if (display.count == 1024) ok = false; // Caller reports failure and releases the session.
    if (ok) { display.input[(display.head + display.count++) % 1024] = e; }
    pthread_mutex_unlock(&display.lock); return ok;
}
static bool lr_kms_display_pop(struct lr_kms_input *e) {
    pthread_mutex_lock(&display.lock);
    if (!display.token) { *e = (struct lr_kms_input){.type = 3}; pthread_mutex_unlock(&display.lock); return true; }
    bool ready = display.count != 0;
    if (ready) { *e = display.input[display.head]; display.head = (display.head + 1) % 1024; display.count--; }
    pthread_mutex_unlock(&display.lock); return ready;
}
