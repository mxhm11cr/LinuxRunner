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

// Native input mailbox ABI (matches fs/kms_display.h; no guest pointers).
struct native_input { uint32_t type, x, y, buttons, keysym, down, width, height; };
#define NATIVE_INPUT_IOCTL 0x802064f0U
_Static_assert(sizeof(struct native_input) == 32, "native input ABI");
static void modifiers(struct wl_proxy *keyboard, struct xkb_state *xkb) {
    wl_proxy_marshal(keyboard, VK_MODIFIERS, xkb_state_serialize_mods(xkb, XKB_STATE_MODS_DEPRESSED),
        xkb_state_serialize_mods(xkb, XKB_STATE_MODS_LATCHED), xkb_state_serialize_mods(xkb, XKB_STATE_MODS_LOCKED),
        xkb_state_serialize_layout(xkb, XKB_STATE_LAYOUT_EFFECTIVE));
}
static void send_key(struct wl_proxy *keyboard, struct xkb_state *xkb, uint32_t code, bool down) {
    xkb_state_update_key(xkb, code, down ? XKB_KEY_DOWN : XKB_KEY_UP);
    wl_proxy_marshal(keyboard, VK_KEY, ms(), code - 8, down ? 1U : 0U);
    modifiers(keyboard, xkb);
}
// Keep text on the same keyboard. Changing keyboards can remove client focus.
// Keep text keycodes within the X11 limit for Xwayland clients.
#define NATIVE_TEXT_SLOTS 32
#define NATIVE_TEXT_FIRST_CODE 224
struct native_text {
    struct wl_proxy *keyboard;
    struct xkb_state *physical;
    uint32_t symbols[NATIVE_TEXT_SLOTS];
    unsigned count, next;
};
static bool text_keymap(struct native_text *text) {
    char *map = NULL;
    size_t size = 0;
    FILE *stream = open_memstream(&map, &size);
    if (!stream) return false;
    fputs("xkb_keymap { xkb_keycodes { include \"evdev+aliases(qwerty)\" ", stream);
    for (unsigned i = 0; i < text->count; i++)
        fprintf(stream, "<L%03u>=%u; ", i, NATIVE_TEXT_FIRST_CODE + i);
    fputs("}; xkb_types { include \"complete\" }; xkb_compatibility { include \"complete\" }; "
        "xkb_symbols { include \"pc+us+inet(evdev)\" ", stream);
    for (unsigned i = 0; i < text->count; i++)
        fprintf(stream, "key <L%03u> { type=\"ONE_LEVEL\", [ 0x%08x ] }; ", i, text->symbols[i]);
    fputs("}; };", stream);
    bool failed = ferror(stream);
    if (fclose(stream) || failed) { free(map); return false; }
    struct xkb_context *context = xkb_context_new(0);
    struct xkb_keymap *compiled = context ? xkb_keymap_new_from_string(context, map, XKB_KEYMAP_FORMAT_TEXT_V1, 0) : NULL;
    char *serialized = compiled ? xkb_keymap_get_as_string(compiled, XKB_KEYMAP_FORMAT_TEXT_V1) : NULL;
    free(map);
    if (compiled) xkb_keymap_unref(compiled);
    if (context) xkb_context_unref(context);
    if (!serialized) return false;
    size = strlen(serialized) + 1;
    int fd = memfd_create("lr-native-text", MFD_CLOEXEC);
    bool ok = fd >= 0 && write(fd, serialized, size) == (ssize_t)size;
    if (ok) {
        wl_proxy_marshal(text->keyboard, VK_KEYMAP, 1U, fd, (uint32_t)size);
        modifiers(text->keyboard, text->physical);
    }
    if (fd >= 0) close(fd);
    free(serialized);
    return ok;
}
static bool send_text(struct native_text *text, uint32_t sym) {
    unsigned slot = 0;
    while (slot < text->count && text->symbols[slot] != sym) slot++;
    if (slot == text->count) {
        struct native_text updated = *text;
        if (updated.count < NATIVE_TEXT_SLOTS) updated.count++;
        else slot = updated.next++ % NATIVE_TEXT_SLOTS;
        updated.symbols[slot] = sym;
        if (!text_keymap(&updated)) return false;
        *text = updated;
    }
    uint32_t code = NATIVE_TEXT_FIRST_CODE + slot - 8;
    wl_proxy_marshal(text->keyboard, VK_KEY, ms(), code, 1U);
    wl_proxy_marshal(text->keyboard, VK_KEY, ms(), code, 0U);
    return true;
}
static int native_loop(int drm, struct wl_display *wl, struct wl_proxy *pointer,
                       struct wl_proxy *keyboard, struct xkb_state *xkb) {
    if (!pointer || !keyboard || !xkb) { fprintf(stderr, "KMS_BRIDGE: native input protocols unavailable\n"); return 1; }
    struct native_text text = {.keyboard = keyboard, .physical = xkb};
    uint32_t held[256] = {0}; bool shifted[256] = {0}; unsigned synthetic_shift = 0;
    uint32_t buttons = 0;
    struct xkb_keymap *map = xkb_state_get_keymap(xkb);
    fprintf(stderr, "KMS_BRIDGE: native input ready (no X11 capture)\n");
    // Readiness is actual compositor protocol connection, independent of pixels.
    const char *ready = getenv("LR_NATIVE_INPUT_READY");
    if (ready) { FILE *f = fopen(ready, "w"); if (f) { fputs("ready\n", f); fclose(f); } }
    for (;;) {
        for (unsigned batch = 0; batch < 256; batch++) {
            struct native_input e = {0};
            if (ioctl(drm, NATIVE_INPUT_IOCTL, &e)) {
                if (errno != EAGAIN) { perror("KMS_BRIDGE: native input"); return 1; }
                break;
            }
            static const uint32_t codes[] = {0x110, 0x112, 0x111};
            if (e.type == 3) {
                for (unsigned k = 8; k < 256; k++) if (held[k]) { send_key(keyboard, xkb, k, false); held[k] = 0; }
                if (synthetic_shift) send_key(keyboard, xkb, 50, false);
                synthetic_shift = 0; memset(shifted, 0, sizeof shifted);
                for (unsigned b = 0; b < 3; b++) if (buttons & (1U << b)) wl_proxy_marshal(pointer, VP_BUTTON, ms(), codes[b], 0U);
                buttons = 0; xkb_state_update_mask(xkb, 0, 0, 0, 0, 0, 0); modifiers(keyboard, xkb);
                wl_proxy_marshal(pointer, VP_FRAME); break;
            } else if (e.type == 1) {
                if (e.width && e.height) wl_proxy_marshal(pointer, VP_MOTION_ABSOLUTE, ms(), e.x, e.y, e.width, e.height);
                for (unsigned b = 0; b < 3; b++) if ((buttons ^ e.buttons) & (1U << b))
                    wl_proxy_marshal(pointer, VP_BUTTON, ms(), codes[b], !!(e.buttons & (1U << b)));
                for (unsigned b = 3; b < 7; b++) if ((e.buttons & ~buttons) & (1U << b)) {
                    int step = b == 3 || b == 5 ? -1 : 1;
                    wl_proxy_marshal(pointer, VP_AXIS_SOURCE, 0U);
                    wl_proxy_marshal(pointer, VP_AXIS_DISCRETE, ms(), b < 5 ? 0U : 1U, wl_fixed_from_int(15 * step), step);
                }
                buttons = e.buttons; wl_proxy_marshal(pointer, VP_FRAME);
            } else if (e.type == 2) {
                uint32_t code = 0; bool shift = false;
                for (unsigned k = 8; k < 256; k++) if (held[k] == e.keysym) { code = k; shift = shifted[k]; break; }
                if (!code && e.down) {
                    for (unsigned level = 0; level < 2 && !code; level++) for (unsigned k = 8; k < 256; k++) {
                        const xkb_keysym_t *syms;
                        int n = xkb_keymap_key_get_syms_by_level(map, k, 0, level, &syms);
                        if (n == 1 && syms[0] == e.keysym) { code = k; shift = level != 0; break; }
                    }
                }
                if (code && e.down && !held[code]) {
                    if (shift && !synthetic_shift++) send_key(keyboard, xkb, 50, true);
                    held[code] = e.keysym; shifted[code] = shift; send_key(keyboard, xkb, code, true);
                } else if (code && !e.down) {
                    send_key(keyboard, xkb, code, false); held[code] = 0; shifted[code] = false;
                    if (shift && synthetic_shift && !--synthetic_shift && !held[50]) send_key(keyboard, xkb, 50, false);
                } else if (e.down && (e.keysym >= 0x1000100U || (e.keysym >= 0xa0 && e.keysym <= 0xff))) {
                    if (!send_text(&text, e.keysym)) fprintf(stderr, "KMS_BRIDGE: native text failed\n");
                }
            }
        }
        // Dispatch compositor events too; a broken connection ends supervision.
        if (wl_display_dispatch_pending(wl) < 0) return 1;
        while (wl_display_prepare_read(wl) != 0) if (wl_display_dispatch_pending(wl) < 0) return 1;
        int flushed = wl_display_flush(wl);
        if (flushed < 0 && errno != EAGAIN) { wl_display_cancel_read(wl); return 1; }
        struct pollfd p = {wl_display_get_fd(wl), POLLIN | (flushed < 0 ? POLLOUT : 0), 0};
        int result = poll(&p, 1, 8);
        if (result > 0 && (p.revents & POLLIN)) { if (wl_display_read_events(wl) < 0) return 1; }
        else wl_display_cancel_read(wl);
        if (result > 0 && (p.revents & (POLLERR | POLLHUP | POLLNVAL))) return 1;
    }
}

int main(int argc, char **argv) {
    const char *node = argc > 1 ? argv[1] : "/dev/dri/card0";
    int drm = open(node, O_RDWR | O_CLOEXEC);
    bool direct = argc > 2 && !strcmp(argv[2], "--native-input");
    Display *x = direct ? NULL : XOpenDisplay(NULL);
    struct wl_display *wl = wl_display_connect(NULL);
    if (drm < 0 || (!direct && !x) || !wl) { fprintf(stderr, "KMS_BRIDGE: open failed drm=%d x=%p wayland=%p\n", drm, (void *) x, (void *) wl); return 1; }
    struct wl_registry *registry = wl_display_get_registry(wl);
    wl_registry_add_listener(registry, &registry_listener, NULL);
    wl_display_roundtrip(wl);
    struct wl_proxy *pointer = pointer_manager ? wl_proxy_marshal_constructor(pointer_manager, 0, &zwlr_virtual_pointer_v1_interface, seat, NULL) : NULL;
    struct wl_proxy *keyboard = keyboard_manager && seat ? wl_proxy_marshal_constructor(keyboard_manager, 0, &zwp_virtual_keyboard_v1_interface, seat, NULL) : NULL;
    struct xkb_state *xkb = keyboard ? send_keymap(keyboard) : NULL;
    wl_display_flush(wl);
    fprintf(stderr, "KMS_BRIDGE: pointer=%s keyboard=%s\n", pointer ? "yes" : "no", xkb ? "yes" : "no");

    if (direct) return native_loop(drm, wl, pointer, keyboard, xkb);

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
