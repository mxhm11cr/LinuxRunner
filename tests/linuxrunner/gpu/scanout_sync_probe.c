// Test-only synchronization of the virtual virgl scanout for X11 capture.
// Not a shipping bridge: no resize, lifecycle, or performance guarantees.
#define _POSIX_C_SOURCE 200809L
#include <fcntl.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>
struct modeinfo { uint32_t clock; uint16_t hdisplay, hsync_start, hsync_end, htotal, hskew, vdisplay, vsync_start, vsync_end, vtotal, vscan; uint32_t vrefresh, flags, type; char name[32]; };
struct mode_crtc { uint64_t set_connectors; uint32_t count_connectors, crtc_id, fb_id, x, y, gamma_size, mode_valid; struct modeinfo mode; };
struct fb_cmd2 { uint32_t fb_id, width, height, pixel_format, flags, handles[4], pitches[4], offsets[4]; uint64_t modifier[4]; };
struct gem_close { uint32_t handle, pad; };
struct virgl_box { uint32_t x, y, z, w, h, d; };
struct virtgpu_transfer { uint32_t handle; struct virgl_box box; uint32_t level, offset, stride, layer_stride; };
#define IOWR(nr, size) (0xc0000000U | (size) << 16 | 'd' << 8 | (nr))
static volatile sig_atomic_t stopped;
static void stop(int signal_number) { (void) signal_number; stopped = 1; }
int main(int argc, char **argv) {
    if (argc != 3) { fprintf(stderr, "usage: scanout_sync_probe DEVICE SECONDS\n"); return 2; }
    char *end; long seconds = strtol(argv[2], &end, 10);
    if (*end || seconds < 1 || seconds > 180) return 2;
    int fd = open(argv[1], O_RDWR | O_CLOEXEC);
    if (fd < 0) { perror("open drm"); return 1; }
    signal(SIGTERM, stop); signal(SIGINT, stop);
    unsigned success = 0, failures = 0;
    struct timespec delay = {.tv_nsec = 100000000};
    for (long i = 0; i < seconds * 10 && !stopped; i++) {
        struct mode_crtc crtc = {.crtc_id = 33};
        struct fb_cmd2 fb = {0};
        if (!ioctl(fd, IOWR(0xa1, sizeof crtc), &crtc) && crtc.mode_valid && crtc.fb_id) {
            fb.fb_id = crtc.fb_id;
            if (!ioctl(fd, IOWR(0xce, sizeof fb), &fb)) {
                struct virtgpu_transfer t = {.handle = fb.handles[0], .box = {0, 0, 0, fb.width, fb.height, 1}, .offset = fb.offsets[0], .stride = fb.pitches[0]};
                if (ioctl(fd, 0xc02c6446U, &t)) failures++; else success++;
                struct gem_close c = {.handle = fb.handles[0]}; ioctl(fd, 0x40086409U, &c);
            } else failures++;
        } else failures++;
        nanosleep(&delay, NULL);
    }
    printf("SCANOUT_SYNC successes=%u failures=%u\n", success, failures);
    close(fd); return success && !failures ? 0 : 1;
}
