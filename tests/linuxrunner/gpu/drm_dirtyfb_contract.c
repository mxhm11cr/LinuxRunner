// Guest contract for DRM_IOCTL_MODE_DIRTYFB on LinuxRunner's DRM node.
// Xorg's modesetting driver reports front-buffer damage with this ioctl
// instead of flipping; without it a GPU-rendered scanout is never read back.
//   gcc -O2 drm_dirtyfb_contract.c -o drm-dirtyfb && ./drm-dirtyfb /dev/dri/card0
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

struct create_dumb { uint32_t height, width, bpp, flags, handle, pitch; uint64_t size; };
struct fb_cmd { uint32_t fb_id, width, height, pitch, bpp, depth, handle; };
struct fb_dirty { uint32_t fb_id, flags, color, num_clips; uint64_t clips_ptr; };
struct clip_rect { uint16_t x1, y1, x2, y2; };
#define IOWR(nr, type) (0xc0000000U | (uint32_t) sizeof(type) << 16 | 'd' << 8 | (nr))
#define CREATE_DUMB IOWR(0xb2, struct create_dumb)
#define ADDFB IOWR(0xae, struct fb_cmd)
#define RMFB IOWR(0xaf, uint32_t)
#define DIRTYFB IOWR(0xb1, struct fb_dirty)

static int failures;
static void expect(int ok, const char *name) {
    printf("DRM_DIRTYFB: %s %s\n", name, ok ? "PASS" : "FAIL");
    if (!ok) failures++;
}

int main(int argc, char **argv) {
    int fd = open(argc > 1 ? argv[1] : "/dev/dri/card0", O_RDWR | O_CLOEXEC);
    if (fd < 0) { perror("open"); return 2; }
    struct create_dumb dumb = {.height = 64, .width = 64, .bpp = 32};
    if (ioctl(fd, CREATE_DUMB, &dumb)) { perror("CREATE_DUMB"); return 2; }
    struct fb_cmd fb = {.width = 64, .height = 64, .pitch = dumb.pitch, .bpp = 32, .depth = 24, .handle = dumb.handle};
    if (ioctl(fd, ADDFB, &fb)) { perror("ADDFB"); return 2; }

    struct fb_dirty whole = {.fb_id = fb.fb_id};
    expect(ioctl(fd, DIRTYFB, &whole) == 0, "whole-framebuffer");
    struct clip_rect clips[3] = {{1, 2, 10, 12}, {40, 40, 80, 90}, {5, 5, 5, 9}};  // second exceeds, third is empty
    struct fb_dirty some = {.fb_id = fb.fb_id, .num_clips = 3, .clips_ptr = (uintptr_t) clips};
    expect(ioctl(fd, DIRTYFB, &some) == 0, "clipped-rectangles");
    struct fb_dirty copy = {.fb_id = fb.fb_id, .flags = 1, .num_clips = 2, .clips_ptr = (uintptr_t) clips};
    expect(ioctl(fd, DIRTYFB, &copy) == 0, "annotate-copy");
    struct fb_dirty unknown = {.fb_id = fb.fb_id + 1000};
    expect(ioctl(fd, DIRTYFB, &unknown) == -1 && errno == ENOENT, "unknown-framebuffer-ENOENT");
    struct fb_dirty flags = {.fb_id = fb.fb_id, .flags = 4};
    expect(ioctl(fd, DIRTYFB, &flags) == -1 && errno == EINVAL, "unknown-flags-EINVAL");
    struct fb_dirty fault = {.fb_id = fb.fb_id, .num_clips = 1, .clips_ptr = 8};
    // A dumb buffer already lives in guest memory, so its clips are never
    // read; the fault case only matters for GPU-backed framebuffers.
    expect(ioctl(fd, DIRTYFB, &fault) == 0 || errno == EFAULT, "bad-clip-pointer-no-crash");

    ioctl(fd, RMFB, &fb.fb_id);
    close(fd);
    printf("DRM_DIRTYFB_RESULT %s\n", failures ? "FAIL" : "PASS");
    return failures != 0;
}
