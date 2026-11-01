// What wlroots/Hyprland do on a DRM node: GBM device -> EGL on the GBM
// platform -> GLES context -> render into a GBM buffer object through an
// FBO -> read pixels back. Reports the renderer actually used.
// Build: gcc gbm_egl_drm_probe.c -lgbm -lEGL -lGLESv2. Prints GBM_EGL PASS.
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>
#include <fcntl.h>
#include <gbm.h>
#include <stdio.h>
#include <unistd.h>

int main(int argc, char **argv) {
    int fd = open(argc > 1 ? argv[1] : "/dev/dri/card0", O_RDWR | O_CLOEXEC);
    if (fd < 0) { perror("GBM_EGL FAIL open"); return 1; }
    struct gbm_device *gbm = gbm_create_device(fd);
    if (!gbm) { puts("GBM_EGL FAIL gbm_create_device"); return 1; }
    printf("GBM_EGL backend=%s\n", gbm_device_get_backend_name(gbm));
    EGLDisplay d = eglGetPlatformDisplay(EGL_PLATFORM_GBM_KHR, gbm, NULL);
    EGLint major, minor;
    if (d == EGL_NO_DISPLAY || !eglInitialize(d, &major, &minor)) { printf("GBM_EGL FAIL eglInitialize 0x%x\n", eglGetError()); return 1; }
    eglBindAPI(EGL_OPENGL_ES_API);
    EGLint attrs[] = {EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE};
    EGLContext ctx = eglCreateContext(d, EGL_NO_CONFIG_KHR, EGL_NO_CONTEXT, attrs);
    if (ctx == EGL_NO_CONTEXT || !eglMakeCurrent(d, EGL_NO_SURFACE, EGL_NO_SURFACE, ctx)) { printf("GBM_EGL FAIL context 0x%x\n", eglGetError()); return 1; }
    printf("GBM_EGL EGL %d.%d renderer=%s version=%s\n", major, minor, glGetString(GL_RENDERER), glGetString(GL_VERSION));
    // Render into a GBM buffer imported as an EGLImage, as a compositor does.
    struct gbm_bo *bo = gbm_bo_create(gbm, 256, 256, GBM_FORMAT_XRGB8888, GBM_BO_USE_RENDERING | GBM_BO_USE_LINEAR);
    if (!bo) { puts("GBM_EGL FAIL gbm_bo_create"); return 1; }
    PFNEGLCREATEIMAGEKHRPROC create_image = (void *) eglGetProcAddress("eglCreateImageKHR");
    PFNGLEGLIMAGETARGETRENDERBUFFERSTORAGEOESPROC target = (void *) eglGetProcAddress("glEGLImageTargetRenderbufferStorageOES");
    int dmabuf = gbm_bo_get_fd(bo);
    EGLint img_attrs[] = {EGL_WIDTH, 256, EGL_HEIGHT, 256, EGL_LINUX_DRM_FOURCC_EXT, GBM_FORMAT_XRGB8888,
        EGL_DMA_BUF_PLANE0_FD_EXT, dmabuf, EGL_DMA_BUF_PLANE0_OFFSET_EXT, 0,
        EGL_DMA_BUF_PLANE0_PITCH_EXT, (EGLint) gbm_bo_get_stride(bo), EGL_NONE};
    EGLImageKHR image = create_image ? create_image(d, EGL_NO_CONTEXT, EGL_LINUX_DMA_BUF_EXT, NULL, img_attrs) : EGL_NO_IMAGE_KHR;
    if (image == EGL_NO_IMAGE_KHR || !target) { printf("GBM_EGL FAIL eglCreateImage 0x%x\n", eglGetError()); return 1; }
    GLuint rb, fb;
    glGenRenderbuffers(1, &rb); glBindRenderbuffer(GL_RENDERBUFFER, rb); target(GL_RENDERBUFFER, image);
    glGenFramebuffers(1, &fb); glBindFramebuffer(GL_FRAMEBUFFER, fb);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, rb);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) { puts("GBM_EGL FAIL framebuffer"); return 1; }
    glViewport(0, 0, 256, 256);
    glClearColor(0.2f, 0.6f, 1.0f, 1.0f); glClear(GL_COLOR_BUFFER_BIT);
    glEnable(GL_SCISSOR_TEST); glScissor(64, 64, 64, 64);
    glClearColor(1.0f, 0.0f, 0.0f, 1.0f); glClear(GL_COLOR_BUFFER_BIT);
    glFinish();
    unsigned char p[4], q[4];
    glReadPixels(10, 10, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, p);
    glReadPixels(80, 80, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, q);
    printf("GBM_EGL pixels background=%d,%d,%d scissor=%d,%d,%d\n", p[0], p[1], p[2], q[0], q[1], q[2]);
    int ok = p[0] == 51 && p[1] == 153 && p[2] == 255 && q[0] == 255 && q[1] == 0 && q[2] == 0;
    puts(ok ? "GBM_EGL PASS" : "GBM_EGL FAIL pixels");
    return !ok;
}
