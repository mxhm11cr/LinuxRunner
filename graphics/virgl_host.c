// Host renderer for LinuxRunner's virtio-gpu node: virglrenderer executing the
// guest's virgl command streams with OpenGL ES contexts from ANGLE's Metal
// backend, so guest GL runs on the Apple GPU. virglrenderer is used through its
// embedder API (contexts from our callbacks, VIRGL_RENDERER_USE_GLES): on Apple
// platforms there is no GBM for its built-in EGL winsys.
// All GL work happens on one GPU thread; guest ioctls from any thread are
// handed to it and wait for completion (including a fence after submissions).
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <EGL/eglext_angle.h>
#include <GLES3/gl3.h>
#include <GLES2/gl2ext.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/uio.h>
#include <unistd.h>
#include <virgl/virglrenderer.h>
#include "graphics/virgl_backend.h"
#include "graphics/virgl_caps.h"

// Private bridge for the transparent-zero border used by desktop samplers.
// ANGLE keeps the full GLES border-color extension unadvertised. virglrenderer
// validates that the border is zero before calling the Metal operation.
// Requires GPU dependencies rebuilt with scripts/gpu/angle-zero-border.py and
// virglrenderer-angle-zero-border.patch (scripts/build-gpu-deps.sh), which
// define LINUXRUNNER_ANGLE_ZERO_BORDER. Older dependency builds keep the
// previous sampler behaviour.
#ifdef LINUXRUNNER_ANGLE_ZERO_BORDER
extern GLboolean LinuxRunnerANGLESetSamplerZeroBorder(GLuint sampler, GLenum pname);
void lr_vrend_set_zero_border_sampler(GLboolean (*operation)(GLuint, GLenum));
#endif

// virglrenderer's public header declares the transfer box opaquely; this is
// its layout (src/virgl_hw.h), part of the transfer ABI.
struct virgl_box { uint32_t x, y, z, w, h, d; };

static EGLDisplay display;
static EGLConfig config;
static EGLContext root;
static EGLSurface pbuffer = EGL_NO_SURFACE;   // only when surfaceless contexts are unavailable
static uint32_t fence_issued, fence_done;
static int cookie;   // virglrenderer passes it back to the callbacks; unused
static bool timer_queries_supported;
static struct iovec **backing;   // per resource handle: virglrenderer keeps the iovec pointer
static uint32_t backing_count;

static void on_fence(void *cookie, uint32_t fence) { (void) cookie; if (fence > fence_done) fence_done = fence; }
static virgl_renderer_gl_context on_create_context(void *cookie, int scanout, struct virgl_renderer_gl_ctx_param *p) {
    (void) cookie; (void) scanout;
    const EGLint attributes[] = {EGL_CONTEXT_MAJOR_VERSION, p->major_ver, EGL_CONTEXT_MINOR_VERSION, p->minor_ver, EGL_NONE};
    // virglrenderer's contexts share objects (resources, fences) with the one
    // current when they are created: its own context 0, which shares with our root.
    EGLContext current = eglGetCurrentContext();
    EGLContext context = eglCreateContext(display, config, current != EGL_NO_CONTEXT ? current : root, attributes);
    return context == EGL_NO_CONTEXT ? NULL : (virgl_renderer_gl_context) context;
}
static void on_destroy_context(void *cookie, virgl_renderer_gl_context context) { (void) cookie; eglDestroyContext(display, (EGLContext) context); }
static int on_make_current(void *cookie, int scanout, virgl_renderer_gl_context context) {
    (void) cookie; (void) scanout;
    return eglMakeCurrent(display, pbuffer, pbuffer, context ? (EGLContext) context : EGL_NO_CONTEXT) ? 0 : -1;
}
static struct virgl_renderer_callbacks callbacks = {
    .version = 1, .write_fence = on_fence, .create_gl_context = on_create_context,
    .destroy_gl_context = on_destroy_context, .make_current = on_make_current,
};

// ---- GPU thread ----------------------------------------------------------------
static pthread_mutex_t call_lock = PTHREAD_MUTEX_INITIALIZER, slot_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t slot_cond = PTHREAD_COND_INITIALIZER;
static void (*job)(void *);
static void *job_arg;
static bool job_done, ready, init_ok;
// What the start found, for the app's Graphics settings: renderer or failure.
static char status[192];
const char *lr_virgl_host_status(void) { return status; }

static void *gpu_main(void *unused) {
    (void) unused;
#ifdef LINUXRUNNER_ANGLE_ZERO_BORDER
    lr_vrend_set_zero_border_sampler(LinuxRunnerANGLESetSamplerZeroBorder);
#endif
    display = eglGetPlatformDisplayEXT(EGL_PLATFORM_ANGLE_ANGLE, (void *) EGL_DEFAULT_DISPLAY,
        (const EGLint[]) {EGL_PLATFORM_ANGLE_TYPE_ANGLE, EGL_PLATFORM_ANGLE_TYPE_METAL_ANGLE, EGL_NONE});
    EGLint major = 0, minor = 0, count = 0;
    bool ok = display != EGL_NO_DISPLAY && eglInitialize(display, &major, &minor) && eglBindAPI(EGL_OPENGL_ES_API) &&
        eglChooseConfig(display, (const EGLint[]) {EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT, EGL_SURFACE_TYPE, EGL_PBUFFER_BIT,
            EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8, EGL_NONE}, &config, 1, &count) && count == 1;
    if (ok) {
        const char *extensions = eglQueryString(display, EGL_EXTENSIONS);
        if (!extensions || !strstr(extensions, "EGL_KHR_surfaceless_context"))
            pbuffer = eglCreatePbufferSurface(display, config, (const EGLint[]) {EGL_WIDTH, 1, EGL_HEIGHT, 1, EGL_NONE});
        root = eglCreateContext(display, config, EGL_NO_CONTEXT, (const EGLint[]) {EGL_CONTEXT_MAJOR_VERSION, 3, EGL_NONE});
        ok = root != EGL_NO_CONTEXT && eglMakeCurrent(display, pbuffer, pbuffer, root);
        if (ok) {
            PFNGLGETQUERYIVEXTPROC query = (void *) eglGetProcAddress("glGetQueryivEXT");
            GLint timestamp = 0, elapsed = 0;
            const char *extensions = (const char *) glGetString(GL_EXTENSIONS);
            if (query && extensions && strstr(extensions, "GL_EXT_disjoint_timer_query")) {
                query(GL_TIMESTAMP_EXT, GL_QUERY_COUNTER_BITS_EXT, &timestamp);
                GLenum timestamp_error = glGetError();
                query(GL_TIME_ELAPSED_EXT, GL_QUERY_COUNTER_BITS_EXT, &elapsed);
                GLenum elapsed_error = glGetError();
                timer_queries_supported = timestamp_error == GL_NO_ERROR && elapsed_error == GL_NO_ERROR && timestamp > 0 && elapsed > 0;
            }
            fprintf(stderr, "VIRGL_HOST: timer queries timestamp_bits=%d elapsed_bits=%d supported=%s\n", timestamp, elapsed, timer_queries_supported ? "yes" : "no");
            ok = virgl_renderer_init(&cookie, VIRGL_RENDERER_USE_GLES, &callbacks) == 0;
        }
    }
    fprintf(stderr, "VIRGL_HOST: EGL %d.%d (%s) renderer=%s init=%s\n", major, minor,
        display ? eglQueryString(display, EGL_VENDOR) : "-", ok ? (const char *) "virglrenderer" : "-", ok ? "ok" : "FAILED");
    if (ok) {
        const char *renderer = (const char *) glGetString(GL_RENDERER);
        snprintf(status, sizeof status, "%s", renderer ? renderer : "ANGLE (Metal)");
    } else if (display == EGL_NO_DISPLAY) snprintf(status, sizeof status, "ANGLE could not open a Metal display");
    else if (!major) snprintf(status, sizeof status, "EGL initialisation failed (error %#x)", eglGetError());
    else if (count != 1) snprintf(status, sizeof status, "no RGBA8 GLES 3 configuration");
    else if (root == EGL_NO_CONTEXT) snprintf(status, sizeof status, "could not create a GLES 3 context (error %#x)", eglGetError());
    else snprintf(status, sizeof status, "virglrenderer did not initialise");
    pthread_mutex_lock(&slot_lock);
    init_ok = ok; ready = true;
    pthread_cond_broadcast(&slot_cond);
    while (true) {
        while (!job) pthread_cond_wait(&slot_cond, &slot_lock);
        void (*run)(void *) = job; void *arg = job_arg;
        pthread_mutex_unlock(&slot_lock);
        // Context creation and global resource operations can change actual
        // GL bindings without going through virglrenderer's guest-context
        // cache. Start every embedder operation on its host context so later
        // submissions switch to the right guest context and retain its FBOs.
        virgl_renderer_force_ctx_0();
        run(arg);
        pthread_mutex_lock(&slot_lock);
        job = NULL; job_done = true;
        pthread_cond_broadcast(&slot_cond);
    }
    return NULL;
}
static void on_gpu(void (*run)(void *), void *arg) {
    pthread_mutex_lock(&call_lock);
    pthread_mutex_lock(&slot_lock);
    job = run; job_arg = arg; job_done = false;
    pthread_cond_broadcast(&slot_cond);
    while (!job_done) pthread_cond_wait(&slot_cond, &slot_lock);
    pthread_mutex_unlock(&slot_lock);
    pthread_mutex_unlock(&call_lock);
}
// Completion of all GPU work issued so far (virgl fence on context 0).
static void finish(void) {
    uint32_t fence = ++fence_issued;
    virgl_renderer_create_fence((int) fence, 0);
    while (fence_done < fence) { virgl_renderer_poll(); if (fence_done < fence) usleep(50); }
}

// ---- backend operations ------------------------------------------------------------
struct op { int result; uint32_t a, b, c, d, e; const void *data; void *out; size_t size; const struct lr_virgl_box *box; uint64_t offset; uint32_t *outp, *outq; const struct lr_virgl_resource_args *args; const char *name; };

static void do_caps(void *p) {
    struct op *o = p; uint32_t max_version = 0, max_size = 0;
    virgl_renderer_get_cap_set(o->a, &max_version, &max_size);
    if (!max_version || o->b > max_version) { o->result = -1; return; }
    void *caps = calloc(1, max_size);
    if (!caps) { o->result = -1; return; }
    virgl_renderer_fill_caps(o->a, o->b, caps);
    lr_virgl_limit_timer_caps(o->a, caps, max_size, timer_queries_supported);
    memcpy(o->out, caps, o->size < max_size ? o->size : max_size);
    *o->outp = max_size;
    free(caps);
    o->result = 0;
}
static int get_caps(uint32_t set, uint32_t version, void *out, uint32_t size, uint32_t *written) {
    struct op o = {.a = set, .b = version, .out = out, .size = size, .outp = written}; on_gpu(do_caps, &o); return o.result;
}
static void do_context_create(void *p) {
    struct op *o = p; o->result = virgl_renderer_context_create(o->a, (uint32_t) strlen(o->name), o->name);
    if (getenv("LR_VIRGL_TRACE")) fprintf(stderr, "VIRGL_HOST: context %u -> %d\n", o->a, o->result);
}
static int context_create(uint32_t ctx, const char *name) { struct op o = {.a = ctx, .name = name}; on_gpu(do_context_create, &o); return o.result; }
static void do_context_destroy(void *p) { virgl_renderer_context_destroy(((struct op *) p)->a); }
static void context_destroy(uint32_t ctx) { struct op o = {.a = ctx}; on_gpu(do_context_destroy, &o); }
static void do_resource_create(void *p) {
    struct op *o = p; const struct lr_virgl_resource_args *a = o->args;
    if (a->handle >= backing_count) {
        uint32_t count = a->handle * 2 + 64;
        struct iovec **grown = realloc(backing, count * sizeof *grown);
        if (!grown) { o->result = -1; return; }
        memset(grown + backing_count, 0, (count - backing_count) * sizeof *grown);
        backing = grown; backing_count = count;
    }
    struct iovec *iov = malloc(sizeof *iov);
    if (!iov) { o->result = -1; return; }
    *iov = (struct iovec) {.iov_base = o->out, .iov_len = o->size};
    struct virgl_renderer_resource_create_args args = {.handle = a->handle, .target = a->target, .format = a->format,
        .bind = a->bind, .width = a->width, .height = a->height, .depth = a->depth, .array_size = a->array_size,
        .last_level = a->last_level, .nr_samples = a->nr_samples, .flags = a->flags};
    // As virtio-gpu does: create, then attach the backing (ATTACH_BACKING), which
    // is what gives the renderer's own resource its guest pages for transfers.
    o->result = virgl_renderer_resource_create(&args, NULL, 0);
    if (!o->result && virgl_renderer_resource_attach_iov((int) a->handle, iov, 1)) {
        virgl_renderer_resource_unref(a->handle);
        o->result = -1;
    }
    if (getenv("LR_VIRGL_TRACE"))
        fprintf(stderr, "VIRGL_HOST: resource %u target %u format %u bind %#x %ux%ux%u layers %u levels %u samples %u flags %#x -> %d\n",
            a->handle, a->target, a->format, a->bind, a->width, a->height, a->depth, a->array_size, a->last_level, a->nr_samples, a->flags, o->result);
    if (o->result) free(iov); else backing[a->handle] = iov;
}
static int resource_create(const struct lr_virgl_resource_args *args, void *map, size_t size) {
    struct op o = {.args = args, .out = map, .size = size}; on_gpu(do_resource_create, &o); return o.result;
}
static void do_resource_unref(void *p) {
    uint32_t handle = ((struct op *) p)->a;
    virgl_renderer_resource_unref(handle);
    if (handle < backing_count) { free(backing[handle]); backing[handle] = NULL; }
}
static void resource_unref(uint32_t handle) { struct op o = {.a = handle}; on_gpu(do_resource_unref, &o); }
static void do_attach(void *p) {
    struct op *o = p; virgl_renderer_ctx_attach_resource((int) o->a, (int) o->b);
    if (getenv("LR_VIRGL_TRACE")) fprintf(stderr, "VIRGL_HOST: attach ctx %u resource %u\n", o->a, o->b);
}
static void context_attach(uint32_t ctx, uint32_t handle) { struct op o = {.a = ctx, .b = handle}; on_gpu(do_attach, &o); }
static void do_submit(void *p) {
    struct op *o = p;
    o->result = virgl_renderer_submit_cmd((void *) o->data, (int) o->a, (int) o->b);
    finish();
    if (getenv("LR_VIRGL_TRACE")) {
        static unsigned submits;
        fprintf(stderr, "VIRGL_HOST: submit %u ctx %u dwords %u -> %d;", ++submits, o->a, o->b, o->result);
        for (uint32_t h = 1; h < backing_count && h < 16; h++) if (backing[h]) {
            const uint8_t *p = backing[h]->iov_base; uint64_t sum = 0; size_t n = backing[h]->iov_len < 65536 ? backing[h]->iov_len : 65536;
            for (size_t i = 0; i < n; i++) sum += p[i];
            fprintf(stderr, " r%u=%llx", h, (unsigned long long) sum);
        }
        fputc('\n', stderr);
    }
}
static int submit(uint32_t ctx, const void *commands, uint32_t dwords) {
    struct op o = {.a = ctx, .b = dwords, .data = commands}; on_gpu(do_submit, &o); return o.result;
}
static void do_transfer(void *p) {
    struct op *o = p;
    struct virgl_box box = {o->box->x, o->box->y, o->box->z, o->box->w, o->box->h, o->box->d};
    if (o->c) o->result = virgl_renderer_transfer_write_iov(o->b, o->a, (int) o->d, o->e, (uint32_t) o->size, &box, o->offset, NULL, 0);
    else { finish(); o->result = virgl_renderer_transfer_read_iov(o->b, o->a, o->d, o->e, (uint32_t) o->size, &box, o->offset, NULL, 0); }
    if (getenv("LR_VIRGL_TRACE"))
        fprintf(stderr, "VIRGL_HOST: transfer %s ctx %u res %u level %u stride %u layer %zu box %u,%u,%u %ux%ux%u offset %llu -> %d\n",
            o->c ? "to-host" : "from-host", o->a, o->b, o->d, o->e, o->size, box.x, box.y, box.z, box.w, box.h, box.d, (unsigned long long) o->offset, o->result);
}
static int transfer(uint32_t ctx, uint32_t handle, int to_host, uint32_t level, uint32_t stride, uint32_t layer_stride,
                    const struct lr_virgl_box *box, uint64_t offset) {
    struct op o = {.a = ctx, .b = handle, .c = (uint32_t) to_host, .d = level, .e = stride, .size = layer_stride, .box = box, .offset = offset};
    on_gpu(do_transfer, &o); return o.result;
}
static void do_texture(void *p) {
    struct op *o = p; struct virgl_renderer_resource_info info;
    if (virgl_renderer_resource_get_info((int) o->a, &info)) { o->result = 0; return; }
    *o->outp = info.width; *o->outq = info.height; o->result = (int) info.tex_id;
}
static uint32_t resource_texture(uint32_t handle, uint32_t *width, uint32_t *height) {
    struct op o = {.a = handle, .outp = width, .outq = height}; on_gpu(do_texture, &o); return (uint32_t) o.result;
}

static const struct lr_virgl_backend backend = {
    .get_caps = get_caps, .context_create = context_create, .context_destroy = context_destroy,
    .resource_create = resource_create, .resource_unref = resource_unref, .context_attach = context_attach,
    .submit = submit, .transfer = transfer, .resource_texture = resource_texture,
};

// Starts the GPU thread; returns the backend once ANGLE and virglrenderer are up, NULL otherwise.
const struct lr_virgl_backend *lr_virgl_host_start(void) {
    pthread_t thread;
    if (pthread_create(&thread, NULL, gpu_main, NULL)) return NULL;
    pthread_detach(thread);
    pthread_mutex_lock(&slot_lock);
    while (!ready) pthread_cond_wait(&slot_cond, &slot_lock);
    bool ok = init_ok;
    pthread_mutex_unlock(&slot_lock);
    return ok ? &backend : NULL;
}
