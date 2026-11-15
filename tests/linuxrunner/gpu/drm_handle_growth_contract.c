// Guest contract: a DRM file holds more than 1,024 GEM handles.
// The handle table used to be a fixed array of 1,024; Mesa's virgl driver in
// a real game (OpenArena) then got ENOSPC from RESOURCE_CREATE, did not handle
// it, and wrote through a NULL mapping. The kernel has no such limit.
//   gcc -O2 drm_handle_growth_contract.c -o drm-handles && ./drm-handles /dev/dri/card1
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/ioctl.h>
#include <unistd.h>

struct create_dumb { uint32_t height, width, bpp, flags, handle, pitch; uint64_t size; };
struct destroy_dumb { uint32_t handle; };
#define IOWR(nr, type) (0xc0000000U | (uint32_t) sizeof(type) << 16 | 'd' << 8 | (nr))
#define CREATE_DUMB IOWR(0xb2, struct create_dumb)
#define DESTROY_DUMB IOWR(0xb4, struct destroy_dumb)
#define COUNT 3000

int main(int argc, char **argv) {
    int fd = open(argc > 1 ? argv[1] : "/dev/dri/card1", O_RDWR | O_CLOEXEC);
    if (fd < 0) { perror("open"); return 2; }
    uint32_t *handles = calloc(COUNT, sizeof *handles);
    int failures = 0, created = 0;
    for (int i = 0; i < COUNT; i++) {
        struct create_dumb c = {.height = 1, .width = 16, .bpp = 32};
        if (ioctl(fd, CREATE_DUMB, &c)) { printf("DRM_HANDLES: create %d failed errno=%d\n", i, errno); failures++; break; }
        handles[created++] = c.handle;
    }
    int unique = 1;
    for (int i = 1; i < created && unique; i++) unique = handles[i] != handles[i - 1];
    printf("DRM_HANDLES: created %d of %d %s\n", created, COUNT, created == COUNT ? "PASS" : "FAIL");
    printf("DRM_HANDLES: distinct-consecutive %s\n", unique ? "PASS" : "FAIL");
    for (int i = 0; i < created; i += 2) {   // free half, then reuse those slots
        struct destroy_dumb d = {handles[i]};
        if (ioctl(fd, DESTROY_DUMB, &d)) failures++;
    }
    int reused = 0;
    for (int i = 0; i < created; i += 2) {
        struct create_dumb c = {.height = 1, .width = 16, .bpp = 32};
        if (ioctl(fd, CREATE_DUMB, &c)) { failures++; break; }
        handles[i] = c.handle; reused++;
    }
    printf("DRM_HANDLES: reuse-after-free %d %s\n", reused, reused == (created + 1) / 2 ? "PASS" : "FAIL");
    for (int i = 0; i < created; i++) { struct destroy_dumb d = {handles[i]}; if (ioctl(fd, DESTROY_DUMB, &d)) failures++; }
    struct destroy_dumb stale = {handles[0]};
    int stale_rejected = ioctl(fd, DESTROY_DUMB, &stale) == -1;
    printf("DRM_HANDLES: stale-handle-rejected %s\n", stale_rejected ? "PASS" : "FAIL");
    close(fd);
    int ok = !failures && created == COUNT && unique && reused == (created + 1) / 2 && stale_rejected;
    printf("DRM_HANDLES_RESULT %s\n", ok ? "PASS" : "FAIL");
    return !ok;
}
