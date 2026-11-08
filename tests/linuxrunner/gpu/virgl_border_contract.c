// Guest pixel contract for the transparent-zero borders used by Xorg glamor.
// Build against the SteamOS sysroot: -lEGL -lGLESv2 -lgbm.
// Run with the isolated virgl Mesa environment and /dev/dri/renderD129.
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES3/gl3.h>
#include <fcntl.h>
#include <gbm.h>
#include <stdio.h>
#include <stdlib.h>

#define CLAMP_TO_BORDER 0x812d
static unsigned failures;

static void check(const char *name, int ok) {
    if (!ok) {
        printf("VIRGL_BORDER FAIL %s gl=%#x egl=%#x\n", name, glGetError(), eglGetError());
        failures++;
    }
}

static GLuint shader(GLenum stage, const char *source) {
    GLuint object = glCreateShader(stage);
    glShaderSource(object, 1, &source, NULL);
    glCompileShader(object);
    GLint ok = 0;
    glGetShaderiv(object, GL_COMPILE_STATUS, &ok);
    check("shader compile", ok);
    return object;
}

static void sample(GLint location, const char *name, GLfloat u, GLfloat v, unsigned expected) {
    glUniform2f(location, u, v);
    glClearColor(1, 0, 1, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    GLubyte pixel[4] = {0};
    glReadPixels(16, 16, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, pixel);
    int ok = 1;
    for (unsigned i = 0; i < 4; i++)
        ok &= abs((int) pixel[i] - (int) expected) <= 1;
    printf("VIRGL_BORDER_PIXEL %s actual=%u,%u,%u,%u expected=%u %s\n",
           name, pixel[0], pixel[1], pixel[2], pixel[3], expected, ok ? "PASS" : "FAIL");
    check(name, ok);
    check("sample GL state", glGetError() == GL_NO_ERROR);
}

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    int fd = open("/dev/dri/renderD129", O_RDWR | O_CLOEXEC);
    struct gbm_device *device = fd >= 0 ? gbm_create_device(fd) : NULL;
    check("GBM device", device != NULL);
    if (failures) return 1;
    EGLDisplay display = eglGetPlatformDisplay(EGL_PLATFORM_GBM_KHR, device, NULL);
    EGLint major = 0, minor = 0, count = 0;
    check("EGL initialize", eglInitialize(display, &major, &minor));
    check("EGL API", eglBindAPI(EGL_OPENGL_ES_API));
    EGLConfig config;
    const EGLint attributes[] = {EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT, EGL_NONE};
    check("EGL config", eglChooseConfig(display, attributes, &config, 1, &count) && count);
    const EGLint context_attributes[] = {EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE};
    EGLContext context = eglCreateContext(display, config, EGL_NO_CONTEXT, context_attributes);
    check("EGL context", context != EGL_NO_CONTEXT &&
          eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, context));
    if (failures) return 1;
    printf("VIRGL_BORDER renderer=%s version=%s\n", glGetString(GL_RENDERER), glGetString(GL_VERSION));

    GLuint target, framebuffer, source, sampler, vertices;
    glGenTextures(1, &target);
    glBindTexture(GL_TEXTURE_2D, target);
    glTexStorage2D(GL_TEXTURE_2D, 1, GL_RGBA8, 32, 32);
    glGenFramebuffers(1, &framebuffer);
    glBindFramebuffer(GL_FRAMEBUFFER, framebuffer);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, target, 0);
    check("framebuffer", glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE);
    glViewport(0, 0, 32, 32);
    glGenTextures(1, &source);
    glBindTexture(GL_TEXTURE_2D, source);
    const GLubyte white[] = {255, 255, 255, 255};
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, white);
    glGenSamplers(1, &sampler);
    glSamplerParameteri(sampler, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glSamplerParameteri(sampler, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glSamplerParameteri(sampler, GL_TEXTURE_WRAP_S, CLAMP_TO_BORDER);
    glSamplerParameteri(sampler, GL_TEXTURE_WRAP_T, CLAMP_TO_BORDER);
    glBindSampler(0, sampler);
    check("sampler parameters", glGetError() == GL_NO_ERROR);

    GLuint program = glCreateProgram();
    glAttachShader(program, shader(GL_VERTEX_SHADER,
        "#version 300 es\nin vec2 position; void main(){gl_Position=vec4(position,0,1);}"));
    glAttachShader(program, shader(GL_FRAGMENT_SHADER,
        "#version 300 es\nprecision highp float; uniform sampler2D image; uniform vec2 sampleAt;"
        "out vec4 color; void main(){color=texture(image,sampleAt);}"));
    glBindAttribLocation(program, 0, "position");
    glLinkProgram(program);
    GLint linked = 0;
    glGetProgramiv(program, GL_LINK_STATUS, &linked);
    check("program link", linked);
    glUseProgram(program);
    glUniform1i(glGetUniformLocation(program, "image"), 0);
    GLint location = glGetUniformLocation(program, "sampleAt");
    const GLfloat quad[] = {-1,-1, 1,-1, -1,1, 1,1};
    glGenBuffers(1, &vertices);
    glBindBuffer(GL_ARRAY_BUFFER, vertices);
    glBufferData(GL_ARRAY_BUFFER, sizeof quad, quad, GL_STATIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, NULL);
    if (failures) return 1;
    sample(location, "nearest-inside", .5f, .5f, 255);
    sample(location, "nearest-outside-left", -.25f, .5f, 0);
    sample(location, "nearest-outside-right", 1.25f, .5f, 0);
    glSamplerParameteri(sampler, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glSamplerParameteri(sampler, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    sample(location, "linear-edge", 0, .5f, 128);
    sample(location, "linear-corner", 0, 0, 64);
    glSamplerParameteri(sampler, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    sample(location, "mixed-edge-border", -.25f, 0, 128);
    glSamplerParameteri(sampler, GL_TEXTURE_WRAP_S, GL_REPEAT);
    glSamplerParameteri(sampler, GL_TEXTURE_WRAP_T, GL_REPEAT);
    sample(location, "repeat-control", -.25f, -.25f, 255);
    printf("VIRGL_BORDER_CONTRACT %s\n", failures ? "FAIL" : "PASS");
    return failures != 0;
}
