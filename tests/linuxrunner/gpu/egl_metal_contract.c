// Mesa EGL (no X server: surfaceless platform) + GLES 2 under the Mesa/Metal
// interposer. Wayland clients and compositors create contexts through EGL, so
// the interposer must track them as it does GLX contexts. Draws into a texture
// framebuffer; run with LR_MESA_METAL_MODE=verify, which draws on Metal and on
// Mesa and compares the pixels (the context release line reports gpu_draws,
// verified and mismatch). Pixels are checked here as well.
// Build: gcc egl_metal_contract.c -lEGL -lGLESv2 -o egl-metal-contract
// Prints EGL_METAL_CONTRACT PASS.
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <stdio.h>
#include <string.h>

static int fails;
static void check(const char *step, int ok) { if (!ok) { printf("EGL_METAL_CONTRACT FAIL %s egl=0x%x gl=0x%x\n", step, eglGetError(), glGetError()); fails++; } }
static GLuint shader(GLenum stage, const char *source) {
    GLuint s = glCreateShader(stage); glShaderSource(s, 1, &source, NULL); glCompileShader(s);
    GLint ok = 0; glGetShaderiv(s, GL_COMPILE_STATUS, &ok); check("compile", ok); return s;
}
static void pixel(int x, int y, unsigned r, unsigned g, unsigned b) {
    unsigned char p[4]; glReadPixels(x, y, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, p);
    printf("EGL_METAL_PIXEL x=%d y=%d actual=%u,%u,%u expected=%u,%u,%u\n", x, y, p[0], p[1], p[2], r, g, b);
    check("pixel", p[0] == r && p[1] == g && p[2] == b);
}

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    EGLDisplay d = eglGetPlatformDisplay(EGL_PLATFORM_SURFACELESS_MESA, EGL_DEFAULT_DISPLAY, NULL);
    EGLint major = 0, minor = 0;
    check("initialize", d != EGL_NO_DISPLAY && eglInitialize(d, &major, &minor));
    eglBindAPI(EGL_OPENGL_ES_API);
    EGLint config_attrs[] = {EGL_SURFACE_TYPE, EGL_PBUFFER_BIT, EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT, EGL_NONE};
    EGLConfig config; EGLint n = 0;
    check("config", eglChooseConfig(d, config_attrs, &config, 1, &n) && n);
    EGLint context_attrs[] = {EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE};
    EGLContext ctx = eglCreateContext(d, config, EGL_NO_CONTEXT, context_attrs);
    check("context", ctx != EGL_NO_CONTEXT && eglMakeCurrent(d, EGL_NO_SURFACE, EGL_NO_SURFACE, ctx));
    if (fails) return 1;
    printf("EGL_METAL_CONTRACT renderer=%s version=%s\n", glGetString(GL_RENDERER), glGetString(GL_VERSION));

    GLuint texture, framebuffer;
    glGenTextures(1, &texture); glBindTexture(GL_TEXTURE_2D, texture);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 64, 64, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glGenFramebuffers(1, &framebuffer); glBindFramebuffer(GL_FRAMEBUFFER, framebuffer);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, texture, 0);
    check("framebuffer complete", glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE);
    glViewport(0, 0, 64, 64);

    GLuint program = glCreateProgram();
    glAttachShader(program, shader(GL_VERTEX_SHADER, "attribute vec2 position; void main() { gl_Position = vec4(position, 0.0, 1.0); }"));
    glAttachShader(program, shader(GL_FRAGMENT_SHADER, "precision mediump float; uniform vec4 color; void main() { gl_FragColor = color; }"));
    glBindAttribLocation(program, 0, "position"); glLinkProgram(program);
    GLint linked = 0; glGetProgramiv(program, GL_LINK_STATUS, &linked); check("link", linked);
    glUseProgram(program);

    // Left half blue, then a green square in the right half.
    static const float left[] = {-1, -1, 0, -1, -1, 1, 0, 1};
    static const float square[] = {0.25f, -0.5f, 0.75f, -0.5f, 0.25f, 0.5f, 0.75f, 0.5f};
    glClearColor(1, 0, 0, 1); glClear(GL_COLOR_BUFFER_BIT);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, left);
    glUniform4f(glGetUniformLocation(program, "color"), 0, 0, 1, 1);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, square);
    glUniform4f(glGetUniformLocation(program, "color"), 0, 1, 0, 1);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    // A textured quad in the bottom-left corner, placed by a mat3 (as a compositor draws client surfaces).
    GLuint image, textured = glCreateProgram();
    static const unsigned char yellow[4] = {255, 255, 0, 255};
    glGenTextures(1, &image); glBindTexture(GL_TEXTURE_2D, image);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, yellow);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST); glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glAttachShader(textured, shader(GL_VERTEX_SHADER, "attribute vec2 position; uniform mat3 transform; varying vec2 uv; void main() { vec2 p = (transform * vec3(position, 1.0)).xy; uv = p * 0.5 + 0.5; gl_Position = vec4(p, 0.0, 1.0); }"));
    glAttachShader(textured, shader(GL_FRAGMENT_SHADER, "precision mediump float; varying vec2 uv; uniform sampler2D image; void main() { gl_FragColor = texture2D(image, uv); }"));
    glBindAttribLocation(textured, 0, "position"); glLinkProgram(textured);
    glGetProgramiv(textured, GL_LINK_STATUS, &linked); check("link textured", linked);
    glUseProgram(textured); glUniform1i(glGetUniformLocation(textured, "image"), 0);
    // mat3 uniform (compositors place surfaces with one): translate a quad at the origin to the corner.
    static const float transform[9] = {1, 0, 0, 0, 1, 0, -1, -1, 1};
    glUniformMatrix3fv(glGetUniformLocation(textured, "transform"), 1, GL_FALSE, transform);
    static const float corner[] = {0, 0, 0.5f, 0, 0, 0.5f, 0.5f, 0.5f};
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, corner);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    glFinish();
    pixel(4, 4, 255, 255, 0);   // textured quad
    pixel(8, 32, 0, 0, 255);    // left half
    pixel(48, 32, 0, 255, 0);   // inside the square
    pixel(48, 4, 255, 0, 0);    // right half, outside the square: clear colour
    check("gl error", glGetError() == GL_NO_ERROR);

    eglMakeCurrent(d, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    eglDestroyContext(d, ctx);   // the interposer reports this context's draws here
    eglTerminate(d);
    if (!fails) puts("EGL_METAL_CONTRACT PASS");
    return fails != 0;
}
