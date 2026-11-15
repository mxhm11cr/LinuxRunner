// Capture the virtual KMS scanout in a disposable guest for comparison with
// X11 capture. Uses the same DRM ABI as lr-kms-bridge; not a production path.
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>
struct modeinfo { uint32_t clock; uint16_t hdisplay, hsync_start, hsync_end, htotal, hskew, vdisplay, vsync_start, vsync_end, vtotal, vscan; uint32_t vrefresh, flags, type; char name[32]; };
struct mode_crtc { uint64_t set_connectors; uint32_t count_connectors, crtc_id, fb_id, x, y, gamma_size, mode_valid; struct modeinfo mode; };
struct fb_cmd2 { uint32_t fb_id, width, height, pixel_format, flags, handles[4], pitches[4], offsets[4]; uint64_t modifier[4]; };
struct map_dumb { uint32_t handle, pad; uint64_t offset; };
struct gem_close { uint32_t handle, pad; };
struct virgl_box { uint32_t x, y, z, w, h, d; };
struct virtgpu_transfer { uint32_t handle; struct virgl_box box; uint32_t level, offset, stride, layer_stride; };
#define IOWR(nr, size) (0xc0000000U | (size) << 16 | 'd' << 8 | (nr))
int main(int argc, char **argv) {
    if ((argc != 3 && argc != 4) || (argc == 4 && strcmp(argv[3], "from-host"))) { fprintf(stderr, "usage: scanout_probe DEVICE OUTPUT [from-host]\n"); return 2; }
    int fd = open(argv[1], O_RDWR | O_CLOEXEC);
    if (fd < 0) { perror("open drm"); return 1; }
    struct mode_crtc crtc = {.crtc_id = 33};
    if (ioctl(fd, IOWR(0xa1, sizeof crtc), &crtc) || !crtc.mode_valid || !crtc.fb_id) {
        perror("get active scanout"); close(fd); return 1;
    }
    struct fb_cmd2 fb = {.fb_id = crtc.fb_id};
    if (ioctl(fd, IOWR(0xce, sizeof fb), &fb)) { perror("get framebuffer"); close(fd); return 1; }
    if (argc == 4) {
        struct virtgpu_transfer transfer = {.handle = fb.handles[0], .box = {0, 0, 0, fb.width, fb.height, 1}, .offset = fb.offsets[0], .stride = fb.pitches[0]};
        int result = ioctl(fd, 0xc02c6446U, &transfer);
        printf("SCANOUT_TRANSFER result=%d errno=%d\n", result, result ? errno : 0);
    }
    struct map_dumb map = {.handle = fb.handles[0]};
    size_t len = (size_t)fb.pitches[0] * fb.height + fb.offsets[0];
    if (!len || len > 256u * 1024u * 1024u || ioctl(fd, IOWR(0xb3, sizeof map), &map)) { perror("map framebuffer"); close(fd); return 1; }
    void *data = mmap(NULL, len, PROT_READ, MAP_SHARED, fd, (off_t)map.offset);
    if (data == MAP_FAILED) { perror("mmap framebuffer"); close(fd); return 1; }
    FILE *out = fopen(argv[2], "wb");
    if (!out) { perror("open output"); munmap(data, len); close(fd); return 1; }
    int ok = fwrite(data, 1, len, out) == len;
    if (fclose(out)) ok = 0;
    printf("SCANOUT width=%u height=%u pitch=%u offset=%u format=0x%x bytes=%zu\n", fb.width, fb.height, fb.pitches[0], fb.offsets[0], fb.pixel_format, len);
    munmap(data, len);
    struct gem_close close_handle = {.handle = fb.handles[0]};
    ioctl(fd, 0x40086409U, &close_handle);
    close(fd); return ok ? 0 : 1;
}
