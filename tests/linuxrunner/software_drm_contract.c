// Software KMS device contract (raw ioctls, no libdrm): version, capabilities,
// dumb buffer create/map/write, PRIME export seen through the dmabuf mapping,
// re-import gives the same handle, cleanup. Node: argv[1] (default
// /dev/dri/card0). Prints DRM_CONTRACT PASS or the first failed step.
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <poll.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

struct version { int32_t major, minor, patch, pad; uint64_t name_len, name, date_len, date, desc_len, desc; };
struct get_cap { uint64_t capability, value; };
struct prime { uint32_t handle, flags; int32_t fd; };
struct create_dumb { uint32_t height, width, bpp, flags, handle, pitch; uint64_t size; };
struct map_dumb { uint32_t handle, pad; uint64_t offset; };
struct card_res { uint64_t fbs, crtcs, connectors, encoders; uint32_t count_fbs, count_crtcs, count_connectors, count_encoders, min_width, max_width, min_height, max_height; };
struct modeinfo { uint32_t clock; uint16_t hdisplay, hsync_start, hsync_end, htotal, hskew, vdisplay, vsync_start, vsync_end, vtotal, vscan; uint32_t vrefresh, flags, type; char name[32]; };
struct mode_crtc { uint64_t set_connectors; uint32_t count_connectors, crtc_id, fb_id, x, y, gamma_size, mode_valid; struct modeinfo mode; };
struct get_connector { uint64_t encoders, modes, props, prop_values; uint32_t count_modes, count_props, count_encoders, encoder_id, connector_id, connector_type, connector_type_id, connection, mm_width, mm_height, subpixel, pad; };
struct fb_cmd2 { uint32_t fb_id, width, height, pixel_format, flags, handles[4], pitches[4], offsets[4]; uint64_t modifier[4]; };
struct page_flip { uint32_t crtc_id, fb_id, flags, reserved; uint64_t user_data; };
struct vblank_event { uint32_t type, length; uint64_t user_data; uint32_t tv_sec, tv_usec, sequence, crtc_id; };
#define IOWR(nr, size) (0xc0000000U | (size) << 16 | 'd' << 8 | (nr))
static int fails;
static void check(const char *step, int ok) { if (!ok) { printf("DRM_CONTRACT FAIL %s errno=%d (%s)\n", step, errno, strerror(errno)); fails++; } }

static double now_ms(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec * 1e3 + t.tv_nsec / 1e6; }

// Legacy modesetting on the virtual output, then reading the scanout from a
// second open as the display bridge does.
static void kms(const char *node) {
    int fd = open(node, O_RDWR | O_CLOEXEC);
    uint32_t crtc = 0, conn = 0, enc = 0;
    struct card_res res = {0};
    check("getresources counts", ioctl(fd, IOWR(0xa0, sizeof res), &res) == 0 && res.count_crtcs == 1 && res.count_connectors == 1);
    res.crtcs = (uintptr_t) &crtc; res.connectors = (uintptr_t) &conn; res.encoders = (uintptr_t) &enc; res.count_fbs = 0;
    check("getresources ids", ioctl(fd, IOWR(0xa0, sizeof res), &res) == 0 && crtc && conn && enc);
    struct modeinfo modes[8]; uint32_t encs[2];
    struct get_connector gc = {.connector_id = conn};
    check("getconnector counts", ioctl(fd, IOWR(0xa7, sizeof gc), &gc) == 0 && gc.connection == 1 && gc.count_modes >= 1);
    gc.modes = (uintptr_t) modes; gc.encoders = (uintptr_t) encs; gc.count_props = 0; gc.count_encoders = 2;
    check("getconnector modes", ioctl(fd, IOWR(0xa7, sizeof gc), &gc) == 0 && (modes[0].type & 8) && modes[0].vrefresh == 60);
    printf("DRM_CONTRACT mode %s\n", modes[0].name);
    unsigned w = modes[0].hdisplay, h = modes[0].vdisplay;
    uint32_t fbid[2], handles[2]; uint32_t *px[2];
    for (int i = 0; i < 2; i++) {
        struct create_dumb c = {.width = w, .height = h, .bpp = 32};
        struct map_dumb m;
        check("scanout dumb", ioctl(fd, IOWR(0xb2, sizeof c), &c) == 0);
        m = (struct map_dumb) {.handle = c.handle};
        ioctl(fd, IOWR(0xb3, sizeof m), &m);
        px[i] = mmap(NULL, c.size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, m.offset);
        for (unsigned y = 0; y < h; y++) for (unsigned x = 0; x < w; x++) px[i][y * (c.pitch / 4) + x] = i ? 0xff00ff00 : 0xff0000ff;
        struct fb_cmd2 f = {.width = w, .height = h, .pixel_format = 0x34325258, .handles = {c.handle}, .pitches = {c.pitch}};
        check("addfb2", ioctl(fd, IOWR(0xb8, sizeof f), &f) == 0 && f.fb_id);
        fbid[i] = f.fb_id; handles[i] = c.handle;
    }
    struct mode_crtc set = {.set_connectors = (uintptr_t) &conn, .count_connectors = 1, .crtc_id = crtc, .fb_id = fbid[0], .mode_valid = 1, .mode = modes[0]};
    check("setcrtc", ioctl(fd, IOWR(0xa2, sizeof set), &set) == 0);
    double t0 = now_ms();
    int flips = 0;
    for (int i = 1; i <= 6; i++) {
        struct page_flip pf = {.crtc_id = crtc, .fb_id = fbid[i & 1], .flags = 1, .user_data = 1000 + i};
        if (ioctl(fd, IOWR(0xb0, sizeof pf), &pf)) break;
        struct pollfd p = {fd, POLLIN, 0};
        struct vblank_event ev;
        if (poll(&p, 1, 1000) == 1 && read(fd, &ev, sizeof ev) == sizeof ev && ev.type == 2 && ev.user_data == 1000u + i && ev.crtc_id == crtc) flips++;
    }
    double per = (now_ms() - t0) / 6;
    check("page flips complete with events", flips == 6);
    printf("DRM_CONTRACT flip interval %.1f ms\n", per);
    check("flips paced near 60 Hz", per > 12 && per < 40);
    // Second open: current scanout through GETCRTC + GETFB2 shows fb 0's pixels.
    int other = open(node, O_RDWR | O_CLOEXEC);
    struct mode_crtc cur = {.crtc_id = crtc};
    check("getcrtc", ioctl(other, IOWR(0xa1, sizeof cur), &cur) == 0 && cur.mode_valid && cur.fb_id == fbid[0]);
    struct fb_cmd2 g = {.fb_id = cur.fb_id};
    check("getfb2", ioctl(other, IOWR(0xce, sizeof g), &g) == 0 && g.handles[0] && g.width == w);
    struct map_dumb gm = {.handle = g.handles[0]};
    uint32_t *view = MAP_FAILED;
    if (ioctl(other, IOWR(0xb3, sizeof gm), &gm) == 0) view = mmap(NULL, (size_t) g.pitches[0] * h, PROT_READ, MAP_SHARED, other, gm.offset);
    check("scanout pixels visible to another open", view != MAP_FAILED && view[0] == 0xff0000ff && view[(h - 1) * (g.pitches[0] / 4) + w - 1] == 0xff0000ff);
    close(fd); // its framebuffers go with it
    struct mode_crtc after = {.crtc_id = crtc};
    check("owner close disables output", ioctl(other, IOWR(0xa1, sizeof after), &after) == 0 && !after.mode_valid && !after.fb_id);
    check("bridge mapping outlives the owner", view != MAP_FAILED && view[1] == 0xff0000ff);
    (void) handles; (void) px;
    close(other);
}

int main(int argc, char **argv) {
    int fd = open(argc > 1 ? argv[1] : "/dev/dri/card0", O_RDWR | O_CLOEXEC);
    check("open", fd >= 0);
    if (fd < 0) return 1;
    char name[64] = {0};
    struct version v = {.name_len = sizeof name - 1, .name = (uintptr_t) name};
    check("version", ioctl(fd, IOWR(0x00, sizeof v), &v) == 0 && !strcmp(name, "linuxrunner") && v.name_len == 11);
    struct get_cap cap = {.capability = 1};
    check("dumb cap", ioctl(fd, IOWR(0x0c, sizeof cap), &cap) == 0 && cap.value == 1);
    cap.capability = 5;
    check("prime cap", ioctl(fd, IOWR(0x0c, sizeof cap), &cap) == 0 && cap.value == 3);
    struct create_dumb c = {.width = 300, .height = 200, .bpp = 32};
    check("create dumb", ioctl(fd, IOWR(0xb2, sizeof c), &c) == 0 && c.handle && c.pitch >= 1200 && c.size >= (uint64_t) c.pitch * 200);
    struct map_dumb m = {.handle = c.handle};
    check("map dumb", ioctl(fd, IOWR(0xb3, sizeof m), &m) == 0);
    uint32_t *px = mmap(NULL, c.size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, m.offset);
    check("mmap dumb", px != MAP_FAILED);
    if (px == MAP_FAILED) return 1;
    for (unsigned i = 0; i < c.size / 4; i++) px[i] = 0xff000000u | i;
    struct prime p = {.handle = c.handle, .flags = O_CLOEXEC | O_RDWR};
    check("prime export", ioctl(fd, IOWR(0x2d, sizeof p), &p) == 0 && p.fd >= 0);
    check("dmabuf size", lseek(p.fd, 0, SEEK_END) == (off_t) c.size);
    uint32_t *view = mmap(NULL, c.size, PROT_READ, MAP_SHARED, p.fd, 0);
    check("mmap dmabuf", view != MAP_FAILED);
    if (view != MAP_FAILED) {
        int same = 1;
        for (unsigned i = 0; i < c.size / 4; i++) if (view[i] != (0xff000000u | i)) { same = 0; break; }
        check("dmabuf shows dumb pixels", same);
        px[17] = 0xdeadbeef;
        check("coherent after write", view[17] == 0xdeadbeef);
        munmap(view, c.size);
    }
    struct prime back = {.fd = p.fd};
    check("prime import same handle", ioctl(fd, IOWR(0x2e, sizeof back), &back) == 0 && back.handle == c.handle);
    int fd2 = open(argc > 1 ? argv[1] : "/dev/dri/card0", O_RDWR | O_CLOEXEC);
    struct prime other = {.fd = p.fd};
    check("import in second open", fd2 >= 0 && ioctl(fd2, IOWR(0x2e, sizeof other), &other) == 0 && other.handle);
    struct map_dumb m2 = {.handle = other.handle};
    uint32_t *again = MAP_FAILED;
    if (ioctl(fd2, IOWR(0xb3, sizeof m2), &m2) == 0) again = mmap(NULL, c.size, PROT_READ, MAP_SHARED, fd2, m2.offset);
    check("imported buffer maps the same pages", again != MAP_FAILED && again[17] == 0xdeadbeef);
    munmap(px, c.size);
    uint32_t h = c.handle;
    check("destroy dumb", ioctl(fd, IOWR(0xb4, 4), &h) == 0);
    check("destroyed handle gone", ioctl(fd, IOWR(0xb3, sizeof m), &m) < 0);
    // The dmabuf and the second open still hold the pages.
    check("pages outlive the handle", again != MAP_FAILED && again[17] == 0xdeadbeef);
    close(p.fd); close(fd2); close(fd);
    kms(argc > 1 ? argv[1] : "/dev/dri/card0");
    if (!fails) printf("DRM_CONTRACT PASS\n");
    return fails != 0;
}
