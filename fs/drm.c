// Software KMS device: the DRM subset that libdrm, GBM and Mesa's kms_swrast
// driver use to allocate CPU-rendered buffers (dumb buffers) and share them
// (PRIME), plus legacy modesetting on one virtual output (connector, encoder,
// CRTC, primary plane) so KMS compositors such as Hyprland can present. It
// lets EGL/GBM clients run with llvmpipe. No display hardware, GPU or fence is
// invented: rendering stays on the CPU, buffers are sealed memfds, a PRIME
// export is the existing software dmabuf of the same pages, and the scanout
// framebuffer is read by LinuxRunner's display bridge (GETCRTC + GETFB2).
// Opt-in: ISH_SOFTWARE_DRM=1 (like the software udmabuf device).
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "kernel/calls.h"
#include "fs/drm.h"
#include "fs/dmabuf.h"
#include "fs/real.h"
#include "fs/poll.h"
#include "graphics/virgl_backend.h"
#include <sys/mman.h>

#define DRM_VERSION_ 0xc0406400U
#define DRM_GET_MAGIC_ 0x80046402U
#define DRM_GEM_CLOSE_ 0x40086409U
#define DRM_GET_CAP_ 0xc010640cU
#define DRM_SET_CLIENT_CAP_ 0x4010640dU
#define DRM_AUTH_MAGIC_ 0x40046411U
#define DRM_SET_MASTER_ 0x0000641eU
#define DRM_DROP_MASTER_ 0x0000641fU
#define DRM_PRIME_HANDLE_TO_FD_ 0xc00c642dU
#define DRM_PRIME_FD_TO_HANDLE_ 0xc00c642eU
#define DRM_MODE_CREATE_DUMB_ 0xc02064b2U
#define DRM_MODE_MAP_DUMB_ 0xc01064b3U
#define DRM_MODE_DESTROY_DUMB_ 0xc00464b4U
#define DRM_MODE_GETRESOURCES_ 0xc04064a0U
#define DRM_MODE_GETCRTC_ 0xc06864a1U
#define DRM_MODE_SETCRTC_ 0xc06864a2U
#define DRM_MODE_CURSOR_ 0xc01c64a3U
#define DRM_MODE_GETGAMMA_ 0xc02064a4U
#define DRM_MODE_SETGAMMA_ 0xc02064a5U
#define DRM_MODE_GETENCODER_ 0xc01464a6U
#define DRM_MODE_GETCONNECTOR_ 0xc05064a7U
#define DRM_MODE_GETPROPERTY_ 0xc04064aaU
#define DRM_MODE_SETPROPERTY_ 0xc01064abU
#define DRM_MODE_GETPROPBLOB_ 0xc01064acU
#define DRM_MODE_GETFB_ 0xc01c64adU
#define DRM_MODE_ADDFB_ 0xc01c64aeU
#define DRM_MODE_RMFB_ 0xc00464afU
#define DRM_MODE_PAGE_FLIP_ 0xc01864b0U
#define DRM_MODE_GETPLANERESOURCES_ 0xc01064b5U
#define DRM_MODE_GETPLANE_ 0xc02064b6U
#define DRM_MODE_SETPLANE_ 0xc03064b7U
#define DRM_MODE_ADDFB2_ 0xc06864b8U
#define DRM_MODE_OBJ_GETPROPERTIES_ 0xc02064b9U
#define DRM_MODE_OBJ_SETPROPERTY_ 0xc01864baU
#define DRM_MODE_CURSOR2_ 0xc02464bbU
#define DRM_MODE_GETFB2_ 0xc06864ceU
#define DRM_CRTC_GET_SEQUENCE_ 0xc018643bU
#define DRM_MODE_CREATE_LEASE_ 0xc01864c6U
#define DRM_MODE_CLOSEFB_ 0xc00864d0U
// virtio-gpu (include/uapi/drm/virtgpu_drm.h), render node LR_VIRGL_RENDER_MINOR
#define VIRTGPU_MAP_ 0xc0106441U
#define VIRTGPU_EXECBUFFER_ 0xc0406442U
#define VIRTGPU_EXECBUFFER_V1_ 0xc0206442U   // before ring_idx/syncobjs (same leading fields)
#define VIRTGPU_GETPARAM_ 0xc0106443U
#define VIRTGPU_RESOURCE_CREATE_ 0xc0386444U
#define VIRTGPU_RESOURCE_INFO_ 0xc0106445U
#define VIRTGPU_TRANSFER_FROM_HOST_ 0xc02c6446U
#define VIRTGPU_TRANSFER_TO_HOST_ 0xc02c6447U
#define VIRTGPU_WAIT_ 0xc0086448U
#define VIRTGPU_GET_CAPS_ 0xc0186449U
#define VIRTGPU_PARAM_3D_FEATURES 1
#define VIRTGPU_PARAM_CAPSET_QUERY_FIX 2
#define VIRTGPU_COMMAND_LIMIT (16U << 20)

#define CAP_DUMB_BUFFER 1
#define CAP_DUMB_PREFERRED_DEPTH 3
#define CAP_DUMB_PREFER_SHADOW 4
#define CAP_PRIME 5
#define CAP_TIMESTAMP_MONOTONIC 6
#define CAP_CURSOR_WIDTH 8
#define CAP_CURSOR_HEIGHT 9
#define CAP_CRTC_IN_VBLANK_EVENT 0x12
#define CLIENT_CAP_STEREO_3D 1
#define CLIENT_CAP_UNIVERSAL_PLANES 2
#define CLIENT_CAP_ASPECT_RATIO 4
#define DRM_CLOEXEC_ 02000000U

#define MAX_HANDLES 1024
#define BUFFER_LIMIT (64ULL << 20)
#define TOTAL_BUDGET (512ULL << 20)

struct version { int32_t major, minor, patch, pad; uint64_t name_len, name, date_len, date, desc_len, desc; };
struct get_cap { uint64_t capability, value; };
struct gem_close { uint32_t handle, pad; };
struct prime { uint32_t handle, flags; int32_t fd; };
struct create_dumb { uint32_t height, width, bpp, flags, handle, pitch; uint64_t size; };
struct map_dumb { uint32_t handle, pad; uint64_t offset; };
struct card_res { uint64_t fbs, crtcs, connectors, encoders; uint32_t count_fbs, count_crtcs, count_connectors, count_encoders, min_width, max_width, min_height, max_height; };
struct modeinfo { uint32_t clock; uint16_t hdisplay, hsync_start, hsync_end, htotal, hskew, vdisplay, vsync_start, vsync_end, vtotal, vscan; uint32_t vrefresh, flags, type; char name[32]; };
struct mode_crtc { uint64_t set_connectors; uint32_t count_connectors, crtc_id, fb_id, x, y, gamma_size, mode_valid; struct modeinfo mode; };
struct get_encoder { uint32_t encoder_id, encoder_type, crtc_id, possible_crtcs, possible_clones; };
struct get_connector { uint64_t encoders, modes, props, prop_values; uint32_t count_modes, count_props, count_encoders, encoder_id, connector_id, connector_type, connector_type_id, connection, mm_width, mm_height, subpixel, pad; };
struct get_property { uint64_t values, enum_blobs; uint32_t prop_id, flags; char name[32]; uint32_t count_values, count_enum_blobs; };
struct property_enum { uint64_t value; char name[32]; };
struct fb_cmd { uint32_t fb_id, width, height, pitch, bpp, depth, handle; };
struct fb_cmd2 { uint32_t fb_id, width, height, pixel_format, flags, handles[4], pitches[4], offsets[4]; uint64_t modifier[4]; };
struct page_flip { uint32_t crtc_id, fb_id, flags, reserved; uint64_t user_data; };
struct plane_res { uint64_t planes; uint32_t count_planes, pad; };
struct get_plane { uint32_t plane_id, crtc_id, fb_id, possible_crtcs, gamma_size, count_format_types; uint64_t format_types; };
struct obj_get_properties { uint64_t props, prop_values; uint32_t count_props, obj_id, obj_type, pad; };
struct obj_set_property { uint64_t value; uint32_t prop_id, obj_id, obj_type, pad; };
struct crtc_sequence { uint32_t crtc_id, active; uint64_t sequence; int64_t sequence_ns; };
struct vblank_event { uint32_t type, length; uint64_t user_data; uint32_t tv_sec, tv_usec, sequence, crtc_id; };
_Static_assert(sizeof(struct card_res) == 64 && sizeof(struct modeinfo) == 68 && sizeof(struct mode_crtc) == 104 &&
    sizeof(struct get_connector) == 80 && sizeof(struct get_property) == 64 && sizeof(struct fb_cmd2) == 104 &&
    sizeof(struct get_plane) == 32 && sizeof(struct obj_get_properties) == 32 && sizeof(struct vblank_event) == 32,
    "struct layouts must match Linux's drm_mode.h");

// A buffer is a page-aligned range of a memfd: its own (dumb) or the one
// behind an imported software dmabuf.
struct buffer {
    atomic_uint refs;
    struct fd *source; // retained
    uint64_t offset, size, map_offset;
    bool dumb;
    // virtio-gpu: host resource and the host mapping of its guest-visible backing.
    uint32_t res_id;
    void *host_map;
    struct buffer *next_virgl; // virgl_buffers list (PRIME imports find the same resource)
};
#define EVENT_QUEUE 16
struct drm_file {
    lock_t lock;
    bool virgl;            // virtio-gpu render node: one host virgl context per open file
    bool ctx_created;
    uint32_t ctx_id;
    struct buffer *handles[MAX_HANDLES]; // handle n is handles[n - 1]
    struct vblank_event events[EVENT_QUEUE];
    unsigned event_count;
};

static atomic_ullong allocated;
static atomic_ullong next_map = 1;

static const struct lr_virgl_backend *virgl;
void lr_virgl_install(const struct lr_virgl_backend *backend) { virgl = backend; }
static lock_t virgl_lock = LOCK_INITIALIZER;   // virgl_buffers
static struct buffer *virgl_buffers;
static atomic_uint next_virgl_ctx = 1, next_virgl_res = 1;

static void buffer_put(struct buffer *b) {
    if (atomic_fetch_sub(&b->refs, 1) != 1) return;
    if (b->res_id) {
        lock(&virgl_lock);
        for (struct buffer **p = &virgl_buffers; *p; p = &(*p)->next_virgl)
            if (*p == b) { *p = b->next_virgl; break; }
        unlock(&virgl_lock);
        virgl->resource_unref(b->res_id);
        if (b->host_map) munmap(b->host_map, b->size);
    }
    if (b->dumb) atomic_fetch_sub(&allocated, b->size);
    fd_close(b->source);
    free(b);
}

static int install_handle(struct drm_file *f, struct buffer *b, uint32_t *handle) {
    for (unsigned i = 0; i < MAX_HANDLES; i++) {
        if (!f->handles[i]) { f->handles[i] = b; *handle = i + 1; return 0; }
    }
    return _ENOSPC;
}
static struct buffer *lookup(struct drm_file *f, uint32_t handle) {
    return handle && handle <= MAX_HANDLES ? f->handles[handle - 1] : NULL;
}

static int create_dumb(struct drm_file *f, struct create_dumb *c) {
    if (!c->width || !c->height || !c->bpp || c->bpp > 32 || c->flags || c->width > 16384 || c->height > 16384)
        return _EINVAL;
    uint64_t pitch = ((uint64_t) c->width * ((c->bpp + 7) / 8) + 63) & ~63ULL;
    uint64_t size = (pitch * c->height + PAGE_SIZE - 1) & ~(uint64_t) (PAGE_SIZE - 1);
    if (size > BUFFER_LIMIT) return _EINVAL;
    unsigned long long old = atomic_load(&allocated);
    do { if (size > TOTAL_BUDGET - old) return _ENOMEM; }
    while (!atomic_compare_exchange_weak(&allocated, &old, old + size));
    int error = 0;
    struct fd *memfd = memfd_new("drm-dumb", 0, &error);
    if (memfd && ftruncate(memfd->real_fd, size) != 0) { error = errno_map(); fd_close(memfd); memfd = NULL; }
    if (!memfd) { atomic_fetch_sub(&allocated, size); return error; }
    // Fixed size, as a GEM object: the software dmabuf exporter requires it.
    memfd->memfd->seals = F_SEAL_SHRINK_ | F_SEAL_GROW_ | F_SEAL_SEAL_;
    struct buffer *b = calloc(1, sizeof *b);
    if (!b) { fd_close(memfd); atomic_fetch_sub(&allocated, size); return _ENOMEM; }
    *b = (struct buffer) {.source = memfd, .size = size, .dumb = true,
        .map_offset = atomic_fetch_add(&next_map, 1) << 32};
    atomic_init(&b->refs, 1);
    lock(&f->lock);
    error = install_handle(f, b, &c->handle);
    unlock(&f->lock);
    if (error) { buffer_put(b); return error; }
    c->pitch = (uint32_t) pitch;
    c->size = size;
    return 0;
}

static int release_handle(struct drm_file *f, uint32_t handle) {
    lock(&f->lock);
    struct buffer *b = lookup(f, handle);
    if (b) f->handles[handle - 1] = NULL;
    unlock(&f->lock);
    if (!b) return _EINVAL;
    buffer_put(b);
    return 0;
}

static int handle_to_fd(struct drm_file *f, struct prime *p) {
    lock(&f->lock);
    struct buffer *b = lookup(f, p->handle);
    struct fd *source = b ? fd_retain(b->source) : NULL;
    uint64_t offset = b ? b->offset : 0, size = b ? b->size : 0;
    unlock(&f->lock);
    if (!source) return _ENOENT;
    int fd = software_dmabuf_export(source, offset, size, p->flags & DRM_CLOEXEC_ ? 1 : 0);
    if (fd < 0) return fd;
    p->fd = fd;
    return 0;
}

static int fd_to_handle(struct drm_file *f, struct prime *p) {
    struct fd *dmabuf = f_get(p->fd);
    struct fd *source;
    uint64_t offset, size;
    if (!dmabuf || !software_dmabuf_source(dmabuf, &source, &offset, &size)) return _EINVAL;
    if (f->virgl) {
        // A virtio-gpu buffer imported again is the same host resource.
        lock(&virgl_lock);
        struct buffer *shared = NULL;
        for (struct buffer *b = virgl_buffers; b; b = b->next_virgl)
            if (b->source == source && b->offset == offset) { shared = b; atomic_fetch_add(&shared->refs, 1); break; }
        unlock(&virgl_lock);
        if (shared) {
            lock(&f->lock);
            for (unsigned i = 0; i < MAX_HANDLES; i++)
                if (f->handles[i] == shared) { p->handle = i + 1; unlock(&f->lock); buffer_put(shared); return 0; }
            int error = install_handle(f, shared, &p->handle);
            unlock(&f->lock);
            if (error) { buffer_put(shared); return error; }
            return 0;
        }
    }
    lock(&f->lock);
    // The same buffer imported twice is the same handle, as in the kernel.
    for (unsigned i = 0; i < MAX_HANDLES; i++) {
        struct buffer *b = f->handles[i];
        if (b && b->source == source && b->offset == offset && b->size == size) {
            p->handle = i + 1;
            unlock(&f->lock);
            return 0;
        }
    }
    struct buffer *b = calloc(1, sizeof *b);
    int error = b ? 0 : _ENOMEM;
    if (b) {
        *b = (struct buffer) {.source = fd_retain(source), .offset = offset, .size = size,
            .map_offset = atomic_fetch_add(&next_map, 1) << 32};
        atomic_init(&b->refs, 1);
        error = install_handle(f, b, &p->handle);
    }
    unlock(&f->lock);
    if (error && b) buffer_put(b);
    return error;
}

static int write_string(uint64_t address, uint64_t *length, const char *text) {
    size_t n = strlen(text);
    if (address && *length && user_write((addr_t) address, text, n < *length ? n : *length))
        return _EFAULT;
    *length = n;
    return 0;
}

// ---- Legacy modesetting on one virtual output -------------------------------
// Object ids are fixed; framebuffers are device-wide like in the kernel.
enum { CONNECTOR_ID = 31, ENCODER_ID = 32, CRTC_ID = 33, PLANE_ID = 34, PROP_TYPE = 40, PROP_DPMS = 41, FB_BASE = 100 };
#define OBJ_CONNECTOR 0xc0c0c0c0U
#define OBJ_CRTC 0xccccccccU
#define OBJ_PLANE 0xeeeeeeeeU
#define FOURCC(a, b, c, d) ((uint32_t) (a) | (uint32_t) (b) << 8 | (uint32_t) (c) << 16 | (uint32_t) (d) << 24)
#define MAX_FBS 64
#define MAX_MODES 4
struct framebuffer { uint32_t width, height, format, pitch, offset; struct buffer *buffer; const void *owner; bool closed; };
static struct {
    lock_t lock;
    struct framebuffer fbs[MAX_FBS];   // fb id FB_BASE + i
    uint32_t scanout;                  // fb id on the CRTC, 0 when off
    bool mode_valid;
    struct modeinfo mode, modes[MAX_MODES];
    unsigned mode_count;
    uint32_t sequence;
    uint64_t last_flip_ns;
} kms;
static pthread_once_t kms_once = PTHREAD_ONCE_INIT;

static void make_mode(struct modeinfo *m, unsigned w, unsigned h, bool preferred) {
    *m = (struct modeinfo) {.hdisplay = w, .hsync_start = w + 48, .hsync_end = w + 80, .htotal = w + 160,
        .vdisplay = h, .vsync_start = h + 3, .vsync_end = h + 8, .vtotal = h + 30, .vrefresh = 60,
        .type = 1 << 6 | (preferred ? 1 << 3 : 0)};
    m->clock = (uint32_t) ((uint64_t) m->htotal * m->vtotal * 60 / 1000);
    snprintf(m->name, sizeof m->name, "%ux%u", w, h);
}
// The first (preferred) mode is the session's display size, so compositors
// that only use listed modes (wlroots/Wayfire) fill the viewer exactly.
static void kms_set_modes(unsigned w, unsigned h) {
    static const unsigned extra[][2] = {{1920, 1080}, {1280, 720}, {1024, 768}};
    kms.mode_count = 0;
    make_mode(&kms.modes[kms.mode_count++], w, h, true);
    for (unsigned i = 0; i < 3 && kms.mode_count < MAX_MODES; i++)
        if (extra[i][0] != w || extra[i][1] != h) make_mode(&kms.modes[kms.mode_count++], extra[i][0], extra[i][1], false);
}
// Start-up size: ISH_SOFTWARE_DRM_MODE=WxH (test runtimes), else 1280x720.
static void kms_init(void) {
    lock_init(&kms.lock);
    unsigned w = 0, h = 0;
    const char *mode = getenv("ISH_SOFTWARE_DRM_MODE");
    if (!mode || sscanf(mode, "%ux%u", &w, &h) != 2 || w < 320 || h < 240 || w > 4096 || h > 4096) { w = 1280; h = 720; }
    kms_set_modes(w, h);
}
// The app sets each desktop session's size before the compositor starts.
void drm_set_display_size(unsigned width, unsigned height) {
    if (width < 320 || height < 240 || width > 4096 || height > 4096) return;
    pthread_once(&kms_once, kms_init);
    lock(&kms.lock);
    kms_set_modes(width, height);
    unlock(&kms.lock);
}

static uint64_t monotonic_ns(void) {
    struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t) t.tv_sec * 1000000000 + t.tv_nsec;
}
static int put_ids(uint64_t address, uint32_t capacity, const uint32_t *ids, uint32_t count) {
    if (address && capacity >= count && count && user_write((addr_t) address, ids, count * sizeof *ids)) return _EFAULT;
    return 0;
}
static struct framebuffer *fb_lookup(uint32_t id) {
    return id >= FB_BASE && id < FB_BASE + MAX_FBS && kms.fbs[id - FB_BASE].buffer && !kms.fbs[id - FB_BASE].closed ? &kms.fbs[id - FB_BASE] : NULL;
}
// A framebuffer removed with CLOSEFB stays on screen until the next frame
// replaces it, as in the kernel; called with kms.lock held, returns the
// buffer to release (after unlocking).
static struct buffer *drop_closed_scanout(uint32_t previous) {
    if (previous < FB_BASE || previous >= FB_BASE + MAX_FBS || previous == kms.scanout) return NULL;
    struct framebuffer *f = &kms.fbs[previous - FB_BASE];
    if (!f->buffer || !f->closed) return NULL;
    struct buffer *b = f->buffer;
    *f = (struct framebuffer) {0};
    return b;
}

static int get_resources(struct card_res *r) {
    uint32_t crtc = CRTC_ID, connector = CONNECTOR_ID, encoder = ENCODER_ID, fbs[MAX_FBS], n = 0;
    lock(&kms.lock);
    for (unsigned i = 0; i < MAX_FBS; i++) if (kms.fbs[i].buffer) fbs[n++] = FB_BASE + i;
    unlock(&kms.lock);
    int err = put_ids(r->fbs, r->count_fbs, fbs, n);
    if (!err) err = put_ids(r->crtcs, r->count_crtcs, &crtc, 1);
    if (!err) err = put_ids(r->connectors, r->count_connectors, &connector, 1);
    if (!err) err = put_ids(r->encoders, r->count_encoders, &encoder, 1);
    r->count_fbs = n; r->count_crtcs = r->count_connectors = r->count_encoders = 1;
    r->min_width = r->min_height = 1; r->max_width = r->max_height = 8192;
    return err;
}

static int get_connector(struct get_connector *c) {
    if (c->connector_id != CONNECTOR_ID) return _ENOENT;
    uint32_t encoder = ENCODER_ID, prop = PROP_DPMS;
    uint64_t dpms = 0;
    if (c->modes && c->count_modes >= kms.mode_count &&
            user_write((addr_t) c->modes, kms.modes, kms.mode_count * sizeof(struct modeinfo))) return _EFAULT;
    int err = put_ids(c->encoders, c->count_encoders, &encoder, 1);
    if (!err) err = put_ids(c->props, c->count_props, &prop, 1);
    if (!err && c->prop_values && c->count_props >= 1 && user_write((addr_t) c->prop_values, &dpms, sizeof dpms)) err = _EFAULT;
    c->count_modes = kms.mode_count; c->count_encoders = 1; c->count_props = 1;
    c->encoder_id = ENCODER_ID;
    c->connector_type = 15; // DRM_MODE_CONNECTOR_VIRTUAL
    c->connector_type_id = 1;
    c->connection = 1;      // connected
    c->mm_width = kms.modes[0].hdisplay * 254 / 960; // 96 DPI
    c->mm_height = kms.modes[0].vdisplay * 254 / 960;
    c->subpixel = 1;
    return err;
}

static void fill_crtc(struct mode_crtc *c) {
    lock(&kms.lock);
    c->fb_id = kms.scanout; c->x = c->y = 0; c->gamma_size = 0;
    c->mode_valid = kms.mode_valid; c->mode = kms.mode;
    unlock(&kms.lock);
}

// A framebuffer rendered by the host GPU (virtio-gpu resource) is copied into
// its shared guest memory once before it is shown, so the display bridge and
// everything after it read the finished frame. The owning file's context is
// taken under kms.lock (drm_close drops a file's framebuffers under it).
static void scanout_sync(uint32_t fb_id) {
    lock(&kms.lock);
    struct framebuffer *fb = fb_lookup(fb_id);
    struct framebuffer copy = fb && fb->buffer->res_id ? *fb : (struct framebuffer) {0};
    const struct drm_file *owner = copy.owner;
    uint32_t ctx = owner && owner->ctx_created ? owner->ctx_id : 0;
    if (copy.buffer) atomic_fetch_add(&copy.buffer->refs, 1);
    unlock(&kms.lock);
    if (!copy.buffer) return;
    struct lr_virgl_box box = {0, 0, 0, copy.width, copy.height, 1};
    int err = virgl->transfer(ctx, copy.buffer->res_id, 0, 0, copy.pitch, 0, &box, copy.offset);
    static atomic_uint reported;
    if (err && atomic_fetch_add(&reported, 1) < 8)
        fprintf(stderr, "VIRGL_SCANOUT: readback of resource %u (%ux%u pitch %u) failed: %d\n", copy.buffer->res_id, copy.width, copy.height, copy.pitch, err);
    buffer_put(copy.buffer);
}

static int set_crtc(struct mode_crtc *c) {
    if (c->crtc_id != CRTC_ID) return _ENOENT;
    if (c->count_connectors > 1) return _EINVAL;
    if (c->count_connectors) {
        uint32_t connector;
        if (user_read((addr_t) c->set_connectors, &connector, sizeof connector)) return _EFAULT;
        if (connector != CONNECTOR_ID) return _ENOENT;
    }
    if (c->mode_valid && c->fb_id != UINT32_MAX) scanout_sync(c->fb_id);
    lock(&kms.lock);
    int err = 0;
    uint32_t previous = kms.scanout;
    if (!c->mode_valid) { kms.mode_valid = false; kms.scanout = 0; }
    else {
        uint32_t fb = c->fb_id == UINT32_MAX ? kms.scanout : c->fb_id;
        struct framebuffer *f = fb_lookup(fb);
        if (!f || f->width < c->mode.hdisplay || f->height < c->mode.vdisplay) err = _EINVAL;
        else { kms.mode = c->mode; kms.mode_valid = true; kms.scanout = fb; }
    }
    struct buffer *dropped = drop_closed_scanout(previous);
    unlock(&kms.lock);
    if (dropped) buffer_put(dropped);
    return err;
}

static void queue_event(struct fd *fd, struct drm_file *f, uint64_t user_data) {
    uint64_t now = monotonic_ns();
    lock(&f->lock);
    if (f->event_count < EVENT_QUEUE)
        f->events[f->event_count++] = (struct vblank_event) {.type = 2 /* FLIP_COMPLETE */, .length = sizeof(struct vblank_event),
            .user_data = user_data, .tv_sec = (uint32_t) (now / 1000000000), .tv_usec = (uint32_t) (now % 1000000000 / 1000),
            .sequence = kms.sequence, .crtc_id = CRTC_ID};
    unlock(&f->lock);
    poll_wakeup(fd, POLL_READ);
}

static int page_flip(struct fd *fd, struct drm_file *f, struct page_flip *p) {
    if (p->crtc_id != CRTC_ID || (p->flags & ~3U)) return _EINVAL;
    lock(&kms.lock);
    bool ok = kms.mode_valid && fb_lookup(p->fb_id);
    uint64_t due = kms.last_flip_ns + 16666667;
    unlock(&kms.lock);
    if (!ok) return _EINVAL;
    scanout_sync(p->fb_id);
    // One flip per refresh: the flip completes at the next 60 Hz slot, so a
    // software compositor is paced as by vblank instead of spinning.
    uint64_t now = monotonic_ns();
    if (now < due) { struct timespec wait = {.tv_nsec = (long) (due - now)}; nanosleep(&wait, NULL); }
    lock(&kms.lock);
    uint32_t previous = kms.scanout;
    kms.scanout = p->fb_id; kms.sequence++; kms.last_flip_ns = monotonic_ns();
    struct buffer *dropped = drop_closed_scanout(previous);
    unlock(&kms.lock);
    if (dropped) buffer_put(dropped);
    if (p->flags & 1) queue_event(fd, f, p->user_data);
    return 0;
}

static int add_fb(struct drm_file *f, uint32_t width, uint32_t height, uint32_t format, uint32_t handle, uint32_t pitch, uint32_t offset, uint32_t *id) {
    if (format != FOURCC('X', 'R', '2', '4') && format != FOURCC('A', 'R', '2', '4')) return _EINVAL;
    if (!width || !height || pitch < width * 4) return _EINVAL;
    lock(&f->lock);
    struct buffer *b = lookup(f, handle);
    if (b) atomic_fetch_add(&b->refs, 1);
    unlock(&f->lock);
    if (!b) return _ENOENT;
    if ((uint64_t) offset + (uint64_t) pitch * height > b->size) { buffer_put(b); return _EINVAL; }
    lock(&kms.lock);
    int err = _ENOSPC;
    for (unsigned i = 0; i < MAX_FBS; i++) {
        if (kms.fbs[i].buffer) continue;
        kms.fbs[i] = (struct framebuffer) {width, height, format, pitch, offset, b, f};
        *id = FB_BASE + i; err = 0; break;
    }
    unlock(&kms.lock);
    if (err) buffer_put(b);
    return err;
}

static int close_fb(uint32_t id) {
    lock(&kms.lock);
    struct framebuffer *f = fb_lookup(id);
    struct buffer *b = NULL;
    if (f && kms.scanout == id) f->closed = true;           // keep showing it
    else if (f) { b = f->buffer; *f = (struct framebuffer) {0}; }
    bool found = f != NULL;
    unlock(&kms.lock);
    if (b) buffer_put(b);
    return found ? 0 : _ENOENT;
}

static int remove_fb(uint32_t id) {
    lock(&kms.lock);
    struct framebuffer *f = fb_lookup(id);
    struct buffer *b = f ? f->buffer : NULL;
    if (f) { *f = (struct framebuffer) {0}; if (kms.scanout == id) { kms.scanout = 0; kms.mode_valid = false; } }
    unlock(&kms.lock);
    if (!b) return _ENOENT;
    buffer_put(b);
    return 0;
}

// GETFB/GETFB2 give the caller its own handle for the framebuffer's buffer;
// LinuxRunner's display bridge reads the scanout this way.
static int get_fb(struct drm_file *f, uint32_t id, struct fb_cmd2 *out) {
    lock(&kms.lock);
    struct framebuffer *fb = fb_lookup(id);
    // A closed framebuffer that is still on screen can be read.
    if (!fb && id == kms.scanout && id >= FB_BASE && id < FB_BASE + MAX_FBS && kms.fbs[id - FB_BASE].buffer)
        fb = &kms.fbs[id - FB_BASE];
    struct framebuffer copy = fb ? *fb : (struct framebuffer) {0};
    if (fb) atomic_fetch_add(&fb->buffer->refs, 1);
    unlock(&kms.lock);
    if (!fb) return _ENOENT;
    lock(&f->lock);
    uint32_t handle = 0;
    int err = install_handle(f, copy.buffer, &handle);
    unlock(&f->lock);
    if (err) { buffer_put(copy.buffer); return err; }
    memset(out, 0, sizeof *out);
    *out = (struct fb_cmd2) {.fb_id = id, .width = copy.width, .height = copy.height, .pixel_format = copy.format};
    out->handles[0] = handle; out->pitches[0] = copy.pitch; out->offsets[0] = copy.offset;
    return 0;
}

static int get_property(struct get_property *p) {
    static const struct property_enum types[] = {{0, "Overlay"}, {1, "Primary"}, {2, "Cursor"}};
    static const struct property_enum dpms[] = {{0, "On"}, {1, "Standby"}, {2, "Suspend"}, {3, "Off"}};
    const struct property_enum *e; uint32_t n;
    if (p->prop_id == PROP_TYPE) { e = types; n = 3; strcpy(p->name, "type"); p->flags = 8 | 4; } // ENUM | IMMUTABLE
    else if (p->prop_id == PROP_DPMS) { e = dpms; n = 4; strcpy(p->name, "DPMS"); p->flags = 8; }
    else return _ENOENT;
    if (p->values && p->count_values >= n)
        for (uint32_t i = 0; i < n; i++) if (user_write((addr_t) p->values + i * 8, &e[i].value, 8)) return _EFAULT;
    if (p->enum_blobs && p->count_enum_blobs >= n && user_write((addr_t) p->enum_blobs, e, n * sizeof *e)) return _EFAULT;
    p->count_values = p->count_enum_blobs = n;
    return 0;
}

static int object_properties(struct obj_get_properties *o) {
    uint32_t prop; uint64_t value; uint32_t n = 1;
    // obj_type 0 is DRM_MODE_OBJECT_ANY: the id alone selects the object.
    bool any = o->obj_type == 0;
    if ((any || o->obj_type == OBJ_PLANE) && o->obj_id == PLANE_ID) { prop = PROP_TYPE; value = 1; } // primary
    else if ((any || o->obj_type == OBJ_CONNECTOR) && o->obj_id == CONNECTOR_ID) { prop = PROP_DPMS; value = 0; }
    else if ((any || o->obj_type == OBJ_CRTC) && o->obj_id == CRTC_ID) { n = 0; prop = 0; value = 0; }
    else return _ENOENT;
    if (n && o->props && o->count_props >= n &&
            (user_write((addr_t) o->props, &prop, 4) || user_write((addr_t) o->prop_values, &value, 8))) return _EFAULT;
    o->count_props = n;
    return 0;
}

static int get_plane(struct get_plane *p) {
    if (p->plane_id != PLANE_ID) return _ENOENT;
    uint32_t formats[] = {FOURCC('X', 'R', '2', '4'), FOURCC('A', 'R', '2', '4')};
    if (p->format_types && p->count_format_types >= 2 && user_write((addr_t) p->format_types, formats, sizeof formats)) return _EFAULT;
    lock(&kms.lock);
    p->crtc_id = kms.mode_valid ? CRTC_ID : 0; p->fb_id = kms.scanout;
    unlock(&kms.lock);
    p->possible_crtcs = 1; p->gamma_size = 0; p->count_format_types = 2;
    return 0;
}

static ssize_t drm_ioctl_size(int command) {
    switch ((uint32_t) command) {
        case DRM_VERSION_: return sizeof(struct version);
        case VIRTGPU_MAP_: case VIRTGPU_GETPARAM_: case VIRTGPU_RESOURCE_INFO_: return 16;
        case VIRTGPU_EXECBUFFER_: return 64;
        case VIRTGPU_EXECBUFFER_V1_: return 32;
        case VIRTGPU_RESOURCE_CREATE_: return 56;
        case VIRTGPU_TRANSFER_FROM_HOST_: case VIRTGPU_TRANSFER_TO_HOST_: return 44;
        case VIRTGPU_WAIT_: return 8;
        case VIRTGPU_GET_CAPS_: return 24;
        case DRM_GET_MAGIC_: case DRM_AUTH_MAGIC_: return 4;
        case DRM_GEM_CLOSE_: return sizeof(struct gem_close);
        case DRM_GET_CAP_: case DRM_SET_CLIENT_CAP_: return sizeof(struct get_cap);
        case DRM_PRIME_HANDLE_TO_FD_: case DRM_PRIME_FD_TO_HANDLE_: return sizeof(struct prime);
        case DRM_MODE_CREATE_DUMB_: return sizeof(struct create_dumb);
        case DRM_MODE_MAP_DUMB_: return sizeof(struct map_dumb);
        case DRM_MODE_DESTROY_DUMB_: return 4;
        case DRM_SET_MASTER_: case DRM_DROP_MASTER_: return 0;
        case DRM_MODE_GETRESOURCES_: return sizeof(struct card_res);
        case DRM_MODE_GETCRTC_: case DRM_MODE_SETCRTC_: return sizeof(struct mode_crtc);
        case DRM_MODE_GETENCODER_: return sizeof(struct get_encoder);
        case DRM_MODE_GETCONNECTOR_: return sizeof(struct get_connector);
        case DRM_MODE_GETPROPERTY_: return sizeof(struct get_property);
        case DRM_MODE_GETPROPBLOB_: case DRM_MODE_SETPROPERTY_: case DRM_MODE_GETPLANERESOURCES_: return 16;
        case DRM_MODE_GETFB_: case DRM_MODE_ADDFB_: return sizeof(struct fb_cmd);
        case DRM_MODE_ADDFB2_: case DRM_MODE_GETFB2_: return sizeof(struct fb_cmd2);
        case DRM_MODE_RMFB_: return 4;
        case DRM_MODE_PAGE_FLIP_: return sizeof(struct page_flip);
        case DRM_MODE_GETPLANE_: return sizeof(struct get_plane);
        case DRM_MODE_SETPLANE_: return 48;
        case DRM_MODE_OBJ_GETPROPERTIES_: return sizeof(struct obj_get_properties);
        case DRM_MODE_OBJ_SETPROPERTY_: return sizeof(struct obj_set_property);
        case DRM_MODE_CURSOR_: return 28;
        case DRM_MODE_CURSOR2_: return 36;
        case DRM_MODE_GETGAMMA_: case DRM_MODE_SETGAMMA_: return 32;
        case DRM_CRTC_GET_SEQUENCE_: return sizeof(struct crtc_sequence);
        case DRM_MODE_CREATE_LEASE_: return 24;
        case DRM_MODE_CLOSEFB_: return 8;
    }
    return -1;
}

static int drm_ioctl_dispatch(struct fd *fd, int command, void *arg);
// ISH_DRM_TRACE=1: one line per ioctl with its result (development only).
static int drm_trace_enabled(void) {
    static int trace = -1;
    if (trace < 0) { const char *v = getenv("ISH_DRM_TRACE"); trace = v && v[0] == '1'; }
    return trace;
}
static int drm_ioctl(struct fd *fd, int command, void *arg) {
    int result = drm_ioctl_dispatch(fd, command, arg);
    if (drm_trace_enabled()) {
        struct drm_file *f = fd->data;
        uint32_t flags = (command & 0xff) == 0xb0 ? ((struct page_flip *) arg)->flags : 0;
        fprintf(stderr, "DRM_IOCTL pid=%d file=%p nr=%#x flags=%#x result=%d scanout=%u seq=%u queued=%u\n", current->pid, (void *) f,
            (uint32_t) command & 0xff, flags, result, kms.scanout, kms.sequence, f ? f->event_count : 0);
    }
    return result;
}

// ---- virtio-gpu (virgl) -----------------------------------------------------
// The guest's Mesa virgl driver submits virgl command streams; the host
// backend (virglrenderer on Metal through ANGLE) executes them. Every call
// completes its GPU work before returning, so the node reports driver version
// 0.0 (no fence fds) and WAIT never blocks.
struct virtgpu_getparam { uint64_t param, value; };
struct virtgpu_resource_create { uint32_t target, format, bind, width, height, depth, array_size, last_level, nr_samples, flags, bo_handle, res_handle, size, stride; };
struct virtgpu_resource_info { uint32_t bo_handle, res_handle, size, blob_mem; };
struct virtgpu_map { uint64_t offset; uint32_t handle, pad; };
struct virtgpu_execbuffer { uint32_t flags, size; uint64_t command, bo_handles; uint32_t num_bo_handles; int32_t fence_fd;
    uint32_t ring_idx, syncobj_stride, num_in_syncobjs, num_out_syncobjs; uint64_t in_syncobjs, out_syncobjs; };
struct virtgpu_transfer { uint32_t bo_handle; struct lr_virgl_box box; uint32_t level, offset, stride, layer_stride; };
struct virtgpu_wait { uint32_t handle, flags; };
struct virtgpu_get_caps { uint32_t cap_set_id, cap_set_ver; uint64_t addr; uint32_t size, pad; };
_Static_assert(sizeof(struct virtgpu_resource_create) == 56 && sizeof(struct virtgpu_execbuffer) == 64 &&
    sizeof(struct virtgpu_transfer) == 44 && sizeof(struct virtgpu_get_caps) == 24, "virtgpu_drm.h layouts");

static void virgl_context(struct drm_file *f) {
    if (f->ctx_created) return;
    f->ctx_created = virgl->context_create(f->ctx_id, "linuxrunner") == 0;
}
// The handle's resource, attached to this file's context (as the kernel does when a GEM object is opened).
static uint32_t virgl_resource(struct drm_file *f, uint32_t handle) {
    lock(&f->lock);
    struct buffer *b = lookup(f, handle);
    uint32_t res = b ? b->res_id : 0;
    unlock(&f->lock);
    if (res) { virgl_context(f); virgl->context_attach(f->ctx_id, res); }
    return res;
}

static int virgl_resource_create(struct drm_file *f, struct virtgpu_resource_create *c) {
    uint64_t size = c->size ? c->size : PAGE_SIZE;
    size = (size + PAGE_SIZE - 1) & ~(uint64_t) (PAGE_SIZE - 1);
    if (size > BUFFER_LIMIT || !c->width) return _EINVAL;
    unsigned long long old = atomic_load(&allocated);
    do { if (size > TOTAL_BUDGET - old) return _ENOMEM; }
    while (!atomic_compare_exchange_weak(&allocated, &old, old + size));
    int error = 0;
    struct fd *memfd = memfd_new("virtio-gpu", 0, &error);
    if (memfd && ftruncate(memfd->real_fd, (off_t) size) != 0) { error = errno_map(); fd_close(memfd); memfd = NULL; }
    void *map = memfd ? mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, memfd->real_fd, 0) : MAP_FAILED;
    if (memfd && map == MAP_FAILED) { error = errno_map(); fd_close(memfd); memfd = NULL; }
    if (!memfd) { atomic_fetch_sub(&allocated, size); return error; }
    memfd->memfd->seals = F_SEAL_SHRINK_ | F_SEAL_GROW_ | F_SEAL_SEAL_;
    struct buffer *b = calloc(1, sizeof *b);
    if (!b) { munmap(map, size); fd_close(memfd); atomic_fetch_sub(&allocated, size); return _ENOMEM; }
    *b = (struct buffer) {.source = memfd, .size = size, .dumb = true, .host_map = map,
        .map_offset = atomic_fetch_add(&next_map, 1) << 32};
    atomic_init(&b->refs, 1);
    struct lr_virgl_resource_args args = {.handle = atomic_fetch_add(&next_virgl_res, 1), .target = c->target, .format = c->format,
        .bind = c->bind, .width = c->width, .height = c->height, .depth = c->depth, .array_size = c->array_size,
        .last_level = c->last_level, .nr_samples = c->nr_samples, .flags = c->flags};
    if (virgl->resource_create(&args, map, size) != 0) { buffer_put(b); return _EINVAL; }
    b->res_id = args.handle;
    lock(&virgl_lock); b->next_virgl = virgl_buffers; virgl_buffers = b; unlock(&virgl_lock);
    lock(&f->lock);
    error = install_handle(f, b, &c->bo_handle);
    unlock(&f->lock);
    if (error) { buffer_put(b); return error; }
    virgl_context(f);
    virgl->context_attach(f->ctx_id, b->res_id);
    c->res_handle = b->res_id;
    return 0;
}

static int virgl_execbuffer(struct drm_file *f, struct virtgpu_execbuffer *e, bool v1) {
    if (e->flags || (!v1 && (e->num_in_syncobjs || e->num_out_syncobjs))) {   // no fences: version 0.0
        fprintf(stderr, "VIRTGPU: execbuffer refused: flags %#x syncobjs in %u out %u\n", e->flags, v1 ? 0 : e->num_in_syncobjs, v1 ? 0 : e->num_out_syncobjs);
        return _EINVAL;
    }
    if (e->size % 4 || e->size > VIRTGPU_COMMAND_LIMIT || e->num_bo_handles > MAX_HANDLES) return _EINVAL;
    for (uint32_t i = 0; i < e->num_bo_handles; i++) {
        uint32_t handle;
        if (user_read((addr_t) (e->bo_handles + 4 * i), &handle, 4)) return _EFAULT;
        if (!virgl_resource(f, handle)) return _ENOENT;
    }
    void *commands = malloc(e->size ? e->size : 4);
    if (!commands) return _ENOMEM;
    if (e->size && user_read((addr_t) e->command, commands, e->size)) { free(commands); return _EFAULT; }
    virgl_context(f);
    int err = e->size ? virgl->submit(f->ctx_id, commands, e->size / 4) : 0;
    if (err) fprintf(stderr, "VIRTGPU: command stream rejected by the renderer (%d), ctx %u, %u dwords\n", err, f->ctx_id, e->size / 4);
    free(commands);
    return err ? _EINVAL : 0;
}

static int virgl_transfer(struct drm_file *f, struct virtgpu_transfer *t, int to_host) {
    uint32_t res = virgl_resource(f, t->bo_handle);
    if (!res) return _ENOENT;
    return virgl->transfer(f->ctx_id, res, to_host, t->level, t->stride, t->layer_stride, &t->box, t->offset) ? _EINVAL : 0;
}

static int virgl_ioctl(struct drm_file *f, uint32_t command, void *arg) {
    switch (command) {
        case VIRTGPU_GETPARAM_: {
            struct virtgpu_getparam *g = arg;
            uint64_t value;
            if (g->param == VIRTGPU_PARAM_3D_FEATURES || g->param == VIRTGPU_PARAM_CAPSET_QUERY_FIX) value = 1;
            else return _EINVAL;   // blob resources, context init, host-visible memory: not offered
            return user_write((addr_t) g->value, &value, sizeof value) ? _EFAULT : 0;
        }
        case VIRTGPU_GET_CAPS_: {
            struct virtgpu_get_caps *c = arg;
            if (c->size > 65536) return _EINVAL;
            void *caps = calloc(1, c->size ? c->size : 1);
            if (!caps) return _ENOMEM;
            uint32_t written = 0;
            int err = virgl->get_caps(c->cap_set_id, c->cap_set_ver, caps, c->size, &written) ? _EINVAL : 0;
            if (!err && c->size && user_write((addr_t) c->addr, caps, c->size)) err = _EFAULT;
            free(caps);
            return err;
        }
        case VIRTGPU_RESOURCE_CREATE_: return virgl_resource_create(f, arg);
        case VIRTGPU_RESOURCE_INFO_: {
            struct virtgpu_resource_info *i = arg;
            lock(&f->lock);
            struct buffer *b = lookup(f, i->bo_handle);
            if (b) { i->res_handle = b->res_id; i->size = (uint32_t) b->size; i->blob_mem = 0; }
            unlock(&f->lock);
            return b && b->res_id ? 0 : _ENOENT;
        }
        case VIRTGPU_MAP_: {
            struct virtgpu_map *m = arg;
            lock(&f->lock);
            struct buffer *b = lookup(f, m->handle);
            if (b) m->offset = b->map_offset;
            unlock(&f->lock);
            return b ? 0 : _ENOENT;
        }
        case VIRTGPU_EXECBUFFER_: return virgl_execbuffer(f, arg, false);
        case VIRTGPU_EXECBUFFER_V1_: return virgl_execbuffer(f, arg, true);
        case VIRTGPU_TRANSFER_TO_HOST_: return virgl_transfer(f, arg, 1);
        case VIRTGPU_TRANSFER_FROM_HOST_: return virgl_transfer(f, arg, 0);
        case VIRTGPU_WAIT_: {
            lock(&f->lock);
            bool known = lookup(f, ((struct virtgpu_wait *) arg)->handle) != NULL;
            unlock(&f->lock);
            return known ? 0 : _ENOENT;   // synchronous backend: never busy
        }
    }
    return _ENOTTY;
}

static int drm_ioctl_dispatch(struct fd *fd, int command, void *arg) {
    struct drm_file *f = fd->data;
    if (f->virgl && ((uint32_t) command & 0xff) >= 0x40 && ((uint32_t) command & 0xff) < 0xa0)
        return virgl_ioctl(f, (uint32_t) command, arg);
    switch ((uint32_t) command) {
        case DRM_VERSION_: {
            struct version *v = arg;
            if (f->virgl) {
                v->major = 0; v->minor = 0; v->patch = 0;   // 0.0: no fence fds (synchronous host)
                int err = write_string(v->name, &v->name_len, "virtio_gpu");
                if (!err) err = write_string(v->date, &v->date_len, "20261101");
                if (!err) err = write_string(v->desc, &v->desc_len, "LinuxRunner virtio-gpu (virgl on Metal)");
                return err;
            }
            v->major = 1; v->minor = 0; v->patch = 0;
            int err = write_string(v->name, &v->name_len, "linuxrunner");
            if (!err) err = write_string(v->date, &v->date_len, "20261101");
            if (!err) err = write_string(v->desc, &v->desc_len, "LinuxRunner software KMS (CPU rendering)");
            return err;
        }
        case DRM_GET_CAP_: {
            struct get_cap *c = arg;
            switch (c->capability) {
                case CAP_DUMB_BUFFER: c->value = 1; return 0;
                case CAP_DUMB_PREFERRED_DEPTH: c->value = 24; return 0;
                case CAP_DUMB_PREFER_SHADOW: c->value = 0; return 0;
                case CAP_PRIME: c->value = 3; return 0; // import and export
                case CAP_TIMESTAMP_MONOTONIC: c->value = 1; return 0;
                case CAP_CURSOR_WIDTH: case CAP_CURSOR_HEIGHT: c->value = 64; return 0;
                case CAP_CRTC_IN_VBLANK_EVENT: c->value = 1; return 0;
            }
            return _EINVAL;
        }
        // Legacy modesetting only: atomic and writeback are refused.
        case DRM_SET_CLIENT_CAP_: {
            uint64_t cap = ((struct get_cap *) arg)->capability;
            return cap == CLIENT_CAP_STEREO_3D || cap == CLIENT_CAP_UNIVERSAL_PLANES || cap == CLIENT_CAP_ASPECT_RATIO ? 0 : _EINVAL;
        }
        case DRM_MODE_GETRESOURCES_: return get_resources(arg);
        case DRM_MODE_GETCONNECTOR_: return get_connector(arg);
        case DRM_MODE_GETENCODER_: {
            struct get_encoder *e = arg;
            if (e->encoder_id != ENCODER_ID) return _ENOENT;
            e->encoder_type = 5; // DRM_MODE_ENCODER_VIRTUAL
            lock(&kms.lock); e->crtc_id = kms.mode_valid ? CRTC_ID : 0; unlock(&kms.lock);
            e->possible_crtcs = 1; e->possible_clones = 0;
            return 0;
        }
        case DRM_MODE_GETCRTC_: {
            struct mode_crtc *c = arg;
            if (c->crtc_id != CRTC_ID) return _ENOENT;
            fill_crtc(c); return 0;
        }
        case DRM_MODE_SETCRTC_: return set_crtc(arg);
        case DRM_MODE_PAGE_FLIP_: return page_flip(fd, f, arg);
        case DRM_MODE_ADDFB_: {
            struct fb_cmd *c = arg;
            if (c->bpp != 32 || (c->depth != 24 && c->depth != 32)) return _EINVAL;
            return add_fb(f, c->width, c->height, c->depth == 32 ? FOURCC('A', 'R', '2', '4') : FOURCC('X', 'R', '2', '4'), c->handle, c->pitch, 0, &c->fb_id);
        }
        case DRM_MODE_ADDFB2_: {
            struct fb_cmd2 *c = arg;
            // DRM_MODE_FB_MODIFIERS: only the linear layout exists here.
            if ((c->flags & 2) && c->modifier[0]) return _EINVAL;
            if (c->handles[1] || c->handles[2] || c->handles[3]) return _EINVAL;
            return add_fb(f, c->width, c->height, c->pixel_format, c->handles[0], c->pitches[0], c->offsets[0], &c->fb_id);
        }
        case DRM_MODE_RMFB_: return remove_fb(*(uint32_t *) arg);
        case DRM_MODE_GETFB2_: { struct fb_cmd2 *c = arg; return get_fb(f, c->fb_id, c); }
        case DRM_MODE_GETFB_: {
            struct fb_cmd *c = arg; struct fb_cmd2 out;
            int err = get_fb(f, c->fb_id, &out);
            if (err) return err;
            *c = (struct fb_cmd) {c->fb_id, out.width, out.height, out.pitches[0], 32, out.pixel_format == FOURCC('A', 'R', '2', '4') ? 32 : 24, out.handles[0]};
            return 0;
        }
        case DRM_MODE_GETPLANERESOURCES_: {
            struct plane_res *p = arg; uint32_t plane = PLANE_ID;
            int err = put_ids(p->planes, p->count_planes, &plane, 1);
            p->count_planes = 1; return err;
        }
        case DRM_MODE_GETPLANE_: return get_plane(arg);
        case DRM_MODE_OBJ_GETPROPERTIES_: return object_properties(arg);
        case DRM_MODE_GETPROPERTY_: return get_property(arg);
        case DRM_MODE_OBJ_SETPROPERTY_: {
            struct obj_set_property *s = arg;
            return s->prop_id == PROP_DPMS && s->obj_id == CONNECTOR_ID && s->value <= 3 ? 0 : _EINVAL;
        }
        case DRM_MODE_SETPROPERTY_: {
            // Legacy connector property: DPMS is the only one, and accepted.
            struct { uint64_t value; uint32_t prop_id, connector_id; } *c = arg;
            return c->prop_id == PROP_DPMS && c->connector_id == CONNECTOR_ID && c->value <= 3 ? 0 : _EINVAL;
        }
        case DRM_MODE_CLOSEFB_: return close_fb(*(uint32_t *) arg);
        case DRM_MODE_GETPROPBLOB_: return _ENOENT; // no EDID or format blobs
        // No hardware cursor plane or gamma table: compositors draw the cursor.
        case DRM_MODE_CURSOR_: case DRM_MODE_CURSOR2_: {
            // Hiding the cursor (no buffer) is valid; showing one needs a plane.
            struct { uint32_t flags, crtc_id, x, y, width, height, handle; } *c = arg;
            return (c->flags & 1) && !c->handle ? 0 : _ENXIO;
        }
        case DRM_MODE_GETGAMMA_: case DRM_MODE_SETGAMMA_: case DRM_MODE_SETPLANE_: return _EINVAL;
        // No DRM leases: clients then open the render node instead.
        case DRM_MODE_CREATE_LEASE_: return _EOPNOTSUPP;
        case DRM_CRTC_GET_SEQUENCE_: {
            struct crtc_sequence *q = arg;
            if (q->crtc_id != CRTC_ID) return _ENOENT;
            lock(&kms.lock);
            q->active = kms.mode_valid; q->sequence = kms.sequence; q->sequence_ns = (int64_t) kms.last_flip_ns;
            unlock(&kms.lock);
            return 0;
        }
        case DRM_GET_MAGIC_: *(uint32_t *) arg = 1; return 0;
        case DRM_AUTH_MAGIC_: case DRM_SET_MASTER_: case DRM_DROP_MASTER_: return 0;
        case DRM_MODE_CREATE_DUMB_: return create_dumb(f, arg);
        case DRM_MODE_MAP_DUMB_: {
            struct map_dumb *m = arg;
            lock(&f->lock);
            struct buffer *b = lookup(f, m->handle);
            if (b) m->offset = b->map_offset;
            unlock(&f->lock);
            return b ? 0 : _ENOENT;
        }
        case DRM_MODE_DESTROY_DUMB_: return release_handle(f, *(uint32_t *) arg);
        case DRM_GEM_CLOSE_: return release_handle(f, ((struct gem_close *) arg)->handle);
        case DRM_PRIME_HANDLE_TO_FD_: return handle_to_fd(f, arg);
        case DRM_PRIME_FD_TO_HANDLE_: return fd_to_handle(f, arg);
    }
    return _ENOTTY;
}

static int drm_mmap(struct fd *fd, struct mem *mem, page_t start, pages_t pages, off_t offset, int prot, int flags) {
    struct drm_file *f = fd->data;
    uint64_t bytes = (uint64_t) pages * PAGE_SIZE;
    if (!(flags & MMAP_SHARED) || offset < 0) return _EINVAL;
    lock(&f->lock);
    struct buffer *found = NULL;
    for (unsigned i = 0; i < MAX_HANDLES && !found; i++) {
        struct buffer *b = f->handles[i];
        if (b && (uint64_t) offset >= b->map_offset && (uint64_t) offset - b->map_offset <= b->size &&
                bytes <= b->size - ((uint64_t) offset - b->map_offset))
            found = b;
    }
    struct fd *source = found ? fd_retain(found->source) : NULL;
    uint64_t at = found ? found->offset + ((uint64_t) offset - found->map_offset) : 0;
    unlock(&f->lock);
    if (!source) return _EINVAL;
    int err = realfs_mmap(source, mem, start, pages, at, prot, flags);
    fd_close(source);
    return err;
}

// Page-flip completion events, as read() returns them on Linux.
static ssize_t drm_read(struct fd *fd, void *buf, size_t size) {
    struct drm_file *f = fd->data;
    size_t done = 0;
    lock(&f->lock);
    unsigned n = 0;
    while (n < f->event_count && done + sizeof(struct vblank_event) <= size) {
        memcpy((char *) buf + done, &f->events[n++], sizeof(struct vblank_event));
        done += sizeof(struct vblank_event);
    }
    memmove(f->events, f->events + n, (f->event_count - n) * sizeof(struct vblank_event));
    f->event_count -= n;
    unsigned left = f->event_count;
    unlock(&f->lock);
    if (drm_trace_enabled()) fprintf(stderr, "DRM_READ pid=%d file=%p events=%u left=%u\n", current->pid, (void *) f, n, left);
    return done ? (ssize_t) done : _EAGAIN;
}
static int drm_poll(struct fd *fd) {
    struct drm_file *f = fd->data;
    lock(&f->lock);
    int ready = f->event_count ? POLL_READ : 0;
    unlock(&f->lock);
    return ready | POLL_WRITE;
}

static int drm_close(struct fd *fd) {
    struct drm_file *f = fd->data;
    if (!f) return 0;
    // A file's framebuffers go with it (and the output if it showed one).
    lock(&kms.lock);
    struct buffer *released[MAX_FBS]; unsigned n = 0;
    for (unsigned i = 0; i < MAX_FBS; i++) {
        if (!kms.fbs[i].buffer || kms.fbs[i].owner != f) continue;
        if (kms.scanout == FB_BASE + i) { kms.scanout = 0; kms.mode_valid = false; }
        released[n++] = kms.fbs[i].buffer;
        kms.fbs[i] = (struct framebuffer) {0};
    }
    unlock(&kms.lock);
    while (n) buffer_put(released[--n]);
    for (unsigned i = 0; i < MAX_HANDLES; i++)
        if (f->handles[i]) buffer_put(f->handles[i]);
    if (f->ctx_created) virgl->context_destroy(f->ctx_id);
    free(f);
    return 0;
}

static int drm_open(int major, int minor, struct fd *fd) {
    (void) major;
    if (minor == LR_VIRGL_RENDER_MINOR || minor == LR_VIRGL_CARD_MINOR) {
        // virtio-gpu (render node, and card1 with the KMS output): only with a host renderer and ISH_VIRGL=1.
        const char *on = getenv("ISH_VIRGL");
        if (!virgl || !on || strcmp(on, "1")) return _ENXIO;
        struct drm_file *f = calloc(1, sizeof *f);
        if (!f) return _ENOMEM;
        lock_init(&f->lock);
        f->virgl = true;
        f->ctx_id = atomic_fetch_add(&next_virgl_ctx, 1);
        if (minor == LR_VIRGL_CARD_MINOR) pthread_once(&kms_once, kms_init);
        fd->data = f;
        return 0;
    }
    const char *enabled = getenv("ISH_SOFTWARE_DRM");
    if ((minor != LR_DRM_CARD_MINOR && minor != LR_DRM_RENDER_MINOR) || !enabled || strcmp(enabled, "1"))
        return _ENXIO;
    pthread_once(&kms_once, kms_init);
    struct drm_file *f = calloc(1, sizeof *f);
    if (!f) return _ENOMEM;
    lock_init(&f->lock);
    fd->data = f;
    return 0;
}

struct dev_ops software_drm_dev = {
    .open = drm_open,
    .fd = {.ioctl_size = drm_ioctl_size, .ioctl = drm_ioctl, .mmap = drm_mmap, .read = drm_read, .poll = drm_poll, .close = drm_close},
};
