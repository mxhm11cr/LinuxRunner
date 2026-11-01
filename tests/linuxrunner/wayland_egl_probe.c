// Minimal Wayland EGL client: connect, EGL on the Wayland platform, a window
// surface, one cleared frame. Mesa's Wayland EGL platform crashed in
// wl_proxy_create_wrapper on LinuxRunner (GTK 4 GL renderer, Qt's Wayland
// EGL); this isolates it from toolkits. Run inside a Wayland compositor.
// Build: gcc wayland_egl_probe.c -lwayland-client -lwayland-egl -lEGL -lGLESv2
// Prints WAYLAND_EGL step lines and WAYLAND_EGL PASS.
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <stdio.h>
#include <string.h>
#include <wayland-client.h>
#include <wayland-egl.h>

static struct wl_compositor *compositor;
static void global(void *data, struct wl_registry *r, uint32_t name, const char *iface, uint32_t version) {
    (void) data; (void) version;
    if (!strcmp(iface, "wl_compositor")) compositor = wl_registry_bind(r, name, &wl_compositor_interface, 4);
}
static void global_remove(void *data, struct wl_registry *r, uint32_t name) { (void) data; (void) r; (void) name; }
static const struct wl_registry_listener listener = {global, global_remove};

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    struct wl_display *display = wl_display_connect(NULL);
    printf("WAYLAND_EGL connect display=%p\n", (void *) display);
    if (!display) return 1;
    struct wl_registry *registry = wl_display_get_registry(display);
    wl_registry_add_listener(registry, &listener, NULL);
    wl_display_roundtrip(display);
    printf("WAYLAND_EGL compositor=%p\n", (void *) compositor);
    EGLDisplay egl = eglGetPlatformDisplay(EGL_PLATFORM_WAYLAND_KHR, display, NULL);
    printf("WAYLAND_EGL platform display=%p\n", egl);
    EGLint major = 0, minor = 0;
    if (!eglInitialize(egl, &major, &minor)) { printf("WAYLAND_EGL FAIL eglInitialize 0x%x\n", eglGetError()); return 1; }
    printf("WAYLAND_EGL initialized %d.%d vendor=%s\n", major, minor, eglQueryString(egl, EGL_VENDOR));
    eglBindAPI(EGL_OPENGL_ES_API);
    EGLint cfg_attrs[] = {EGL_SURFACE_TYPE, EGL_WINDOW_BIT, EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT, EGL_RED_SIZE, 8, EGL_NONE};
    EGLConfig config; EGLint n = 0;
    if (!eglChooseConfig(egl, cfg_attrs, &config, 1, &n) || !n) { puts("WAYLAND_EGL FAIL config"); return 1; }
    EGLint ctx_attrs[] = {EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE};
    EGLContext ctx = eglCreateContext(egl, config, EGL_NO_CONTEXT, ctx_attrs);
    struct wl_surface *surface = wl_compositor_create_surface(compositor);
    struct wl_egl_window *window = wl_egl_window_create(surface, 200, 150);
    EGLSurface s = eglCreatePlatformWindowSurface(egl, config, window, NULL);
    printf("WAYLAND_EGL surface=%p context=%p\n", s, ctx);
    if (s == EGL_NO_SURFACE || !eglMakeCurrent(egl, s, s, ctx)) { printf("WAYLAND_EGL FAIL current 0x%x\n", eglGetError()); return 1; }
    printf("WAYLAND_EGL renderer=%s\n", glGetString(GL_RENDERER));
    glClearColor(0, 0.5f, 1, 1); glClear(GL_COLOR_BUFFER_BIT);
    if (!eglSwapBuffers(egl, s)) { printf("WAYLAND_EGL FAIL swap 0x%x\n", eglGetError()); return 1; }
    puts("WAYLAND_EGL PASS");
    return 0;
}
