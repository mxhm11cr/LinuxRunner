// LinuxRunner KMS display bridge: shows the software DRM device's scanout in a
// full-screen window of the session's X server (so the viewer, input and
// external-display path stay the same) and sends that window's pointer and
// keyboard input to the Wayland compositor through the standard virtual
// pointer / virtual keyboard protocols (wlr-virtual-pointer-unstable-v1,
// virtual-keyboard-unstable-v1). Used for KMS compositors such as Hyprland.
// Build: gcc lr-kms-bridge.c -lX11 -lXext -lwayland-client -lxkbcommon
// Usage: lr-kms-bridge /dev/dri/card0   (DISPLAY and WAYLAND_DISPLAY set)
#define _GNU_SOURCE
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/extensions/XShm.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/ipc.h>
#include <sys/mman.h>
#include <sys/shm.h>
#include <time.h>
#include <unistd.h>
#include <wayland-client.h>
#include <xkbcommon/xkbcommon.h>

// ---- DRM (the subset of drm_mode.h the bridge reads) ------------------------
struct modeinfo { uint32_t clock; uint16_t hdisplay, hsync_start, hsync_end, htotal, hskew, vdisplay, vsync_start, vsync_end, vtotal, vscan; uint32_t vrefresh, flags, type; char name[32]; };
struct mode_crtc { uint64_t set_connectors; uint32_t count_connectors, crtc_id, fb_id, x, y, gamma_size, mode_valid; struct modeinfo mode; };
struct fb_cmd2 { uint32_t fb_id, width, height, pixel_format, flags, handles[4], pitches[4], offsets[4]; uint64_t modifier[4]; };
struct map_dumb { uint32_t handle, pad; uint64_t offset; };
struct gem_close { uint32_t handle, pad; };
#define IOWR(nr, size) (0xc0000000U | (size) << 16 | 'd' << 8 | (nr))
#define CRTC_ID 33

// ---- Wayland virtual input interfaces (as wayland-scanner would emit) -------
static const struct wl_interface zwlr_virtual_pointer_v1_interface;
static const struct wl_interface zwp_virtual_keyboard_v1_interface;
static const struct wl_interface *vp_create_types[] = {&wl_seat_interface, &zwlr_virtual_pointer_v1_interface};
static const struct wl_interface *vk_create_types[] = {&wl_seat_interface, &zwp_virtual_keyboard_v1_interface};
static const struct wl_interface *no_types[8];
static const struct wl_message vp_manager_requests[] = {{"create_virtual_pointer", "?on", vp_create_types}, {"destroy", "", no_types}};
static const struct wl_interface zwlr_virtual_pointer_manager_v1_interface = {"zwlr_virtual_pointer_manager_v1", 1, 2, vp_manager_requests, 0, NULL};
static const struct wl_message vp_requests[] = {
    {"motion", "uff", no_types}, {"motion_absolute", "uuuuu", no_types}, {"button", "uuu", no_types},
    {"axis", "uuf", no_types}, {"frame", "", no_types}, {"axis_source", "u", no_types},
    {"axis_stop", "uu", no_types}, {"axis_discrete", "uufi", no_types}, {"destroy", "", no_types}};
static const struct wl_interface zwlr_virtual_pointer_v1_interface = {"zwlr_virtual_pointer_v1", 1, 9, vp_requests, 0, NULL};
static const struct wl_message vk_manager_requests[] = {{"create_virtual_keyboard", "on", vk_create_types}};
static const struct wl_interface zwp_virtual_keyboard_manager_v1_interface = {"zwp_virtual_keyboard_manager_v1", 1, 1, vk_manager_requests, 0, NULL};
static const struct wl_message vk_requests[] = {{"keymap", "uhu", no_types}, {"key", "uuu", no_types}, {"modifiers", "uuuu", no_types}, {"destroy", "", no_types}};
static const struct wl_interface zwp_virtual_keyboard_v1_interface = {"zwp_virtual_keyboard_v1", 1, 4, vk_requests, 0, NULL};
enum { VP_MOTION_ABSOLUTE = 1, VP_BUTTON = 2, VP_AXIS = 3, VP_FRAME = 4, VP_AXIS_SOURCE = 5, VP_AXIS_DISCRETE = 7 };
enum { VK_KEYMAP = 0, VK_KEY = 1, VK_MODIFIERS = 2 };

static struct wl_seat *seat;
static struct wl_proxy *pointer_manager, *keyboard_manager;
static void global(void *data, struct wl_registry *r, uint32_t name, const char *iface, uint32_t version) {
    (void) data; (void) version;
    if (!strcmp(iface, "wl_seat") && !seat) seat = wl_registry_bind(r, name, &wl_seat_interface, 1);
    else if (!strcmp(iface, zwlr_virtual_pointer_manager_v1_interface.name)) pointer_manager = wl_registry_bind(r, name, &zwlr_virtual_pointer_manager_v1_interface, 1);
    else if (!strcmp(iface, zwp_virtual_keyboard_manager_v1_interface.name)) keyboard_manager = wl_registry_bind(r, name, &zwp_virtual_keyboard_manager_v1_interface, 1);
}
static void global_remove(void *data, struct wl_registry *r, uint32_t name) { (void) data; (void) r; (void) name; }
static const struct wl_registry_listener registry_listener = {global, global_remove};

static uint32_t ms(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (uint32_t) (t.tv_sec * 1000 + t.tv_nsec / 1000000); }

// A US keymap from xkbcommon's defaults, as the virtual keyboard's keymap.
static struct xkb_state *send_keymap(struct wl_proxy *keyboard) {
    struct xkb_context *ctx = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
    struct xkb_keymap *map = ctx ? xkb_keymap_new_from_names(ctx, NULL, XKB_KEYMAP_COMPILE_NO_FLAGS) : NULL;
    char *text = map ? xkb_keymap_get_as_string(map, XKB_KEYMAP_FORMAT_TEXT_V1) : NULL;
    if (!text) return NULL;
    size_t size = strlen(text) + 1;
    int fd = memfd_create("lr-kms-bridge-keymap", MFD_CLOEXEC);
    if (fd < 0 || write(fd, text, size) != (ssize_t) size) { free(text); return NULL; }
    wl_proxy_marshal(keyboard, VK_KEYMAP, 1 /* XKB_V1 */, fd, (uint32_t) size);
    close(fd); free(text);
    return xkb_state_new(map);
}

int main(int argc, char **argv) {
    const char *node = argc > 1 ? argv[1] : "/dev/dri/card0";
    int drm = open(node, O_RDWR | O_CLOEXEC);
    Display *x = XOpenDisplay(NULL);
    struct wl_display *wl = wl_display_connect(NULL);
    if (drm < 0 || !x || !wl) { fprintf(stderr, "KMS_BRIDGE: open failed drm=%d x=%p wayland=%p\n", drm, (void *) x, (void *) wl); return 1; }
    struct wl_registry *registry = wl_display_get_registry(wl);
    wl_registry_add_listener(registry, &registry_listener, NULL);
    wl_display_roundtrip(wl);
    struct wl_proxy *pointer = pointer_manager ? wl_proxy_marshal_constructor(pointer_manager, 0, &zwlr_virtual_pointer_v1_interface, seat, NULL) : NULL;
    struct wl_proxy *keyboard = keyboard_manager && seat ? wl_proxy_marshal_constructor(keyboard_manager, 0, &zwp_virtual_keyboard_v1_interface, seat, NULL) : NULL;
    struct xkb_state *xkb = keyboard ? send_keymap(keyboard) : NULL;
    wl_display_flush(wl);
    fprintf(stderr, "KMS_BRIDGE: pointer=%s keyboard=%s\n", pointer ? "yes" : "no", xkb ? "yes" : "no");

    int screen = DefaultScreen(x);
    unsigned w = DisplayWidth(x, screen), h = DisplayHeight(x, screen);
    XSetWindowAttributes attrs = {.override_redirect = True, .background_pixel = BlackPixel(x, screen),
        .event_mask = ButtonPressMask | ButtonReleaseMask | PointerMotionMask | KeyPressMask | KeyReleaseMask | ExposureMask};
    Window win = XCreateWindow(x, RootWindow(x, screen), 0, 0, w, h, 0, CopyFromParent, InputOutput, CopyFromParent,
                               CWOverrideRedirect | CWBackPixel | CWEventMask, &attrs);
    XStoreName(x, win, "LinuxRunner KMS output");
    // The compositor draws its own pointer into the output: hide the X one
    // over this window, or two pointers show.
    char blank_bits[1] = {0};
    Pixmap blank = XCreateBitmapFromData(x, win, blank_bits, 1, 1);
    XColor black = {0};
    Cursor none = XCreatePixmapCursor(x, blank, blank, &black, &black, 0, 0);
    XDefineCursor(x, win, none);
    XFreePixmap(x, blank);
    XMapRaised(x, win);
    XSetInputFocus(x, win, RevertToParent, CurrentTime);
    GC gc = DefaultGC(x, screen);
    XShmSegmentInfo shm = {0};
    XImage *image = NULL;
    if (XShmQueryExtension(x)) {
        image = XShmCreateImage(x, DefaultVisual(x, screen), 24, ZPixmap, NULL, &shm, w, h);
        shm.shmid = image ? shmget(IPC_PRIVATE, (size_t) image->bytes_per_line * h, IPC_CREAT | 0600) : -1;
        shm.shmaddr = shm.shmid >= 0 ? shmat(shm.shmid, NULL, 0) : (void *) -1;
        if (image) image->data = shm.shmaddr;
        if (!image || shm.shmaddr == (void *) -1 || !XShmAttach(x, &shm)) {
            if (image) { image->data = NULL; XDestroyImage(image); }
            image = NULL; shm.shmaddr = NULL;
        } else shmctl(shm.shmid, IPC_RMID, NULL);
    }
    if (!image) image = XCreateImage(x, DefaultVisual(x, screen), 24, ZPixmap, 0, malloc((size_t) w * h * 4), w, h, 32, 0);
    fprintf(stderr, "KMS_BRIDGE: window %ux%u %s\n", w, h, shm.shmaddr && shm.shmaddr != (void *) -1 ? "MIT-SHM" : "XPutImage");
    XSync(x, False);

    uint32_t shown_fb = 0, frames = 0;
    bool debug = getenv("LR_KMS_BRIDGE_DEBUG") != NULL;
    unsigned long shown_sequence = 0;
    for (;;) {
        // Input first: X events become virtual pointer / keyboard requests.
        while (XPending(x)) {
            XEvent e; XNextEvent(x, &e);
            if (debug) fprintf(stderr, "KMS_BRIDGE: X event %d\n", e.type);
            if (e.type == Expose) { shown_fb = 0; continue; }
            if (pointer && e.type == MotionNotify) {
                wl_proxy_marshal(pointer, VP_MOTION_ABSOLUTE, ms(), (uint32_t) e.xmotion.x, (uint32_t) e.xmotion.y, w, h);
                wl_proxy_marshal(pointer, VP_FRAME);
            } else if (pointer && (e.type == ButtonPress || e.type == ButtonRelease)) {
                unsigned b = e.xbutton.button;
                wl_proxy_marshal(pointer, VP_MOTION_ABSOLUTE, ms(), (uint32_t) e.xbutton.x, (uint32_t) e.xbutton.y, w, h);
                if (b >= 4 && b <= 7) {
                    if (e.type == ButtonPress) {
                        uint32_t axis = b <= 5 ? 0 : 1; int step = b == 4 || b == 6 ? -1 : 1;
                        wl_proxy_marshal(pointer, VP_AXIS_SOURCE, 0 /* wheel */);
                        wl_proxy_marshal(pointer, VP_AXIS_DISCRETE, ms(), axis, wl_fixed_from_int(15 * step), step);
                    }
                } else {
                    static const uint32_t codes[] = {0, 0x110, 0x112, 0x111}; // BTN_LEFT, BTN_MIDDLE, BTN_RIGHT
                    if (b <= 3) wl_proxy_marshal(pointer, VP_BUTTON, ms(), codes[b], e.type == ButtonPress ? 1 : 0);
                }
                wl_proxy_marshal(pointer, VP_FRAME);
            } else if (xkb && (e.type == KeyPress || e.type == KeyRelease)) {
                // X keycodes are evdev codes + 8 under Xvfb's evdev rules.
                bool down = e.type == KeyPress;
                xkb_state_update_key(xkb, e.xkey.keycode, down ? XKB_KEY_DOWN : XKB_KEY_UP);
                wl_proxy_marshal(keyboard, VK_KEY, ms(), e.xkey.keycode - 8, down ? 1 : 0);
                wl_proxy_marshal(keyboard, VK_MODIFIERS, xkb_state_serialize_mods(xkb, XKB_STATE_MODS_DEPRESSED),
                                 xkb_state_serialize_mods(xkb, XKB_STATE_MODS_LATCHED), xkb_state_serialize_mods(xkb, XKB_STATE_MODS_LOCKED),
                                 xkb_state_serialize_layout(xkb, XKB_STATE_LAYOUT_EFFECTIVE));
            }
        }
        if (wl_display_flush(wl) < 0 && errno != EAGAIN) { fprintf(stderr, "KMS_BRIDGE: compositor connection closed\n"); return 0; }
        // Output: copy the scanout when the compositor flipped to a new frame.
        struct mode_crtc crtc = {.crtc_id = CRTC_ID};
        struct { uint32_t crtc_id, active; uint64_t sequence; int64_t ns; } seq = {.crtc_id = CRTC_ID};
        ioctl(drm, IOWR(0x3b, sizeof seq), &seq);
        if (ioctl(drm, IOWR(0xa1, sizeof crtc), &crtc) == 0 && crtc.mode_valid && crtc.fb_id &&
                (crtc.fb_id != shown_fb || seq.sequence != shown_sequence)) {
            struct fb_cmd2 fb = {.fb_id = crtc.fb_id};
            if (ioctl(drm, IOWR(0xce, sizeof fb), &fb) == 0) {
                struct map_dumb map = {.handle = fb.handles[0]};
                size_t bytes = (size_t) fb.pitches[0] * fb.height + fb.offsets[0];
                uint8_t *src = MAP_FAILED;
                if (ioctl(drm, IOWR(0xb3, sizeof map), &map) == 0)
                    src = mmap(NULL, bytes, PROT_READ, MAP_SHARED, drm, (off_t) map.offset);
                if (src != MAP_FAILED) {
                    unsigned cw = fb.width < w ? fb.width : w, ch = fb.height < h ? fb.height : h;
                    for (unsigned y = 0; y < ch; y++)
                        memcpy(image->data + (size_t) y * image->bytes_per_line, src + fb.offsets[0] + (size_t) y * fb.pitches[0], (size_t) cw * 4);
                    munmap(src, bytes);
                    if (shm.shmaddr && shm.shmaddr != (void *) -1) XShmPutImage(x, win, gc, image, 0, 0, 0, 0, cw, ch, False);
                    else XPutImage(x, win, gc, image, 0, 0, 0, 0, cw, ch);
                    XFlush(x);
                    shown_fb = crtc.fb_id; shown_sequence = seq.sequence;
                    if (++frames == 1) fprintf(stderr, "KMS_BRIDGE: first frame %ux%u fb=%u\n", fb.width, fb.height, fb.fb_id);
                }
                struct gem_close close_handle = {.handle = fb.handles[0]};
                ioctl(drm, 0x40086409U, &close_handle);
            }
        }
        // ~60 Hz: wake for X input or the next possible frame.
        struct pollfd p = {ConnectionNumber(x), POLLIN, 0};
        poll(&p, 1, 16);
    }
}
