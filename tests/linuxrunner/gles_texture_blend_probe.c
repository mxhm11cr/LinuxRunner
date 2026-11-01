// llvmpipe pixel-pipeline probe: textured quad (nearest sampling) and alpha
// blending through Mesa's JIT-compiled shaders, read back and compared with
// the exact expected bytes. glClear alone (egl_dmabuf_probe) does not run
// shader or blend code; this does.
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <stdio.h>
#include <string.h>

static GLuint shader(GLenum type, const char *src) {
    GLuint s = glCreateShader(type);
    glShaderSource(s, 1, &src, NULL);
    glCompileShader(s);
    return s;
}

int main(void) {
    EGLDisplay d = eglGetPlatformDisplay(EGL_PLATFORM_SURFACELESS_MESA, EGL_DEFAULT_DISPLAY, NULL);
    if (!eglInitialize(d, NULL, NULL)) { puts("GLES_PROBE_FAIL egl-init"); return 2; }
    EGLint cfgattr[] = {EGL_SURFACE_TYPE, EGL_PBUFFER_BIT, EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
                        EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8, EGL_NONE};
    EGLConfig cfg; EGLint n;
    eglChooseConfig(d, cfgattr, &cfg, 1, &n);
    EGLint pb[] = {EGL_WIDTH, 4, EGL_HEIGHT, 4, EGL_NONE};
    EGLSurface surf = eglCreatePbufferSurface(d, cfg, pb);
    eglBindAPI(EGL_OPENGL_ES_API);
    EGLint ctxattr[] = {EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE};
    EGLContext c = eglCreateContext(d, cfg, EGL_NO_CONTEXT, ctxattr);
    if (!eglMakeCurrent(d, surf, surf, c)) { puts("GLES_PROBE_FAIL make-current"); return 2; }
    printf("GLES_RENDERER %s\n", glGetString(GL_RENDERER));

    GLuint p = glCreateProgram();
    glAttachShader(p, shader(GL_VERTEX_SHADER,
        "attribute vec2 pos; varying vec2 uv; void main(){ uv = pos*0.5+0.5; gl_Position = vec4(pos,0.0,1.0); }"));
    glAttachShader(p, shader(GL_FRAGMENT_SHADER,
        "precision mediump float; varying vec2 uv; uniform sampler2D t; uniform float k;"
        " void main(){ vec4 c = texture2D(t, uv); gl_FragColor = vec4(c.rgb * k, c.a); }"));
    glBindAttribLocation(p, 0, "pos");
    glLinkProgram(p);
    glUseProgram(p);
    // 4x4 texture: rows of white, red, (64,128,191), translucent white (a=128).
    unsigned char tex[4 * 4 * 4];
    const unsigned char rows[4][4] = {{255,255,255,255},{255,0,0,255},{64,128,191,255},{255,255,255,128}};
    for (int y = 0; y < 4; y++) for (int x = 0; x < 4; x++) memcpy(tex + (y * 4 + x) * 4, rows[y], 4);
    GLuint t; glGenTextures(1, &t); glBindTexture(GL_TEXTURE_2D, t);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 4, 4, 0, GL_RGBA, GL_UNSIGNED_BYTE, tex);
    glUniform1f(glGetUniformLocation(p, "k"), 1.0f);
    const float quad[] = {-1,-1, 1,-1, -1,1, 1,1};
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, quad);
    glEnableVertexAttribArray(0);
    glViewport(0, 0, 4, 4);

    int failures = 0;
    unsigned char out[4 * 4 * 4];
    // 1) plain textured draw
    glDisable(GL_BLEND); glClearColor(0, 0, 0, 0); glClear(GL_COLOR_BUFFER_BIT);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    glReadPixels(0, 0, 4, 4, GL_RGBA, GL_UNSIGNED_BYTE, out);
    for (int y = 0; y < 4; y++) {
        printf("GLES_TEXTURE row%d got %3u %3u %3u %3u want %3u %3u %3u %3u\n", y, out[y*16], out[y*16+1], out[y*16+2], out[y*16+3],
               rows[y][0], rows[y][1], rows[y][2], rows[y][3]);
        for (int k = 0; k < 4; k++) if (out[y*16+k] > rows[y][k] + 1 || out[y*16+k] + 1 < rows[y][k]) failures++;
    }
    // 2) premultiplied "over" blend of the translucent row onto opaque grey (as KWin composites)
    glClearColor(0.5f, 0.5f, 0.5f, 1.0f); glClear(GL_COLOR_BUFFER_BIT);
    glEnable(GL_BLEND); glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
    glUniform1f(glGetUniformLocation(p, "k"), 0.5f);   // premultiply rgb by 0.5 for the a=128 texel
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    glReadPixels(0, 0, 4, 4, GL_RGBA, GL_UNSIGNED_BYTE, out);
    // row 3: src (127.5,127.5,127.5,128) over (128,128,128,255): 127.5 + 128*(1-128/255) = ~191
    printf("GLES_BLEND row3 got %u %u %u %u want ~191 191 191 255\n", out[48], out[49], out[50], out[51]);
    for (int k = 0; k < 3; k++) if (out[48+k] < 188 || out[48+k] > 194) failures++;
    if (out[51] < 254) failures++;
    // 3) KWin's shm upload path on GLES: BGRA texture (GL_EXT_texture_format_BGRA8888),
    //    updated with glTexSubImage2D from a wider buffer (GL_UNPACK_ROW_LENGTH_EXT).
    glDisable(GL_BLEND);
    glUniform1f(glGetUniformLocation(p, "k"), 1.0f);
    unsigned char bgra[8 * 4 * 4];   // 8-pixel-wide source rows, 4 used
    memset(bgra, 0xEE, sizeof bgra);
    for (int y = 0; y < 4; y++) for (int x = 0; x < 4; x++) {
        unsigned char *q = bgra + (y * 8 + x) * 4;
        q[0] = rows[y][2]; q[1] = rows[y][1]; q[2] = rows[y][0]; q[3] = rows[y][3];
    }
    glTexImage2D(GL_TEXTURE_2D, 0, 0x80E1 /* GL_BGRA_EXT */, 4, 4, 0, 0x80E1, GL_UNSIGNED_BYTE, NULL);
    glPixelStorei(0x0CF2 /* GL_UNPACK_ROW_LENGTH_EXT */, 8);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 4, 4, 0x80E1, GL_UNSIGNED_BYTE, bgra);
    glPixelStorei(0x0CF2, 0);
    printf("GLES_BGRA_UPLOAD error=%#x\n", glGetError());
    glClearColor(0, 0, 0, 0); glClear(GL_COLOR_BUFFER_BIT);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    glReadPixels(0, 0, 4, 4, GL_RGBA, GL_UNSIGNED_BYTE, out);
    for (int y = 0; y < 4; y++) {
        printf("GLES_BGRA row%d got %3u %3u %3u %3u want %3u %3u %3u %3u\n", y, out[y*16], out[y*16+1], out[y*16+2], out[y*16+3],
               rows[y][0], rows[y][1], rows[y][2], rows[y][3]);
        for (int k = 0; k < 4; k++) if (out[y*16+k] > rows[y][k] + 1 || out[y*16+k] + 1 < rows[y][k]) failures++;
    }
    // 4) Colour-management style math (KWin converts sRGB <-> linear): pow, exp2,
    //    log2 and the sRGB EOTF/OETF pair, on known inputs.
    GLuint q = glCreateProgram();
    glAttachShader(q, shader(GL_VERTEX_SHADER,
        "attribute vec2 pos; void main(){ gl_Position = vec4(pos,0.0,1.0); }"));
    glAttachShader(q, shader(GL_FRAGMENT_SHADER,
        "precision highp float; uniform vec4 v; uniform int mode;"
        " vec3 eotf(vec3 c){ return mix(c/12.92, pow((c+0.055)/1.055, vec3(2.4)), step(vec3(0.04045), c)); }"
        " vec3 oetf(vec3 c){ return mix(c*12.92, 1.055*pow(c, vec3(1.0/2.4))-0.055, step(vec3(0.0031308), c)); }"
        " void main(){ vec3 r;"
        "  if(mode==0) r = pow(v.rgb, vec3(2.2));"
        "  else if(mode==1) r = exp2(log2(v.rgb));"
        "  else if(mode==2) r = oetf(eotf(v.rgb));"
        "  else r = eotf(v.rgb);"
        "  gl_FragColor = vec4(r, 1.0); }"));
    glBindAttribLocation(q, 0, "pos");
    glLinkProgram(q);
    glUseProgram(q);
    const float in[3] = {0.25f, 0.5f, 0.75f};
    // expected bytes: pow(x,2.2); identity; identity; sRGB EOTF
    const float expect[4][3] = {{0.04736f,0.21764f,0.53100f},{0.25f,0.5f,0.75f},{0.25f,0.5f,0.75f},{0.05099f,0.21404f,0.52252f}};
    const char *names[4] = {"pow2.2","exp2(log2)","oetf(eotf)","srgb-eotf"};
    for (int mode = 0; mode < 4; mode++) {
        glUniform4f(glGetUniformLocation(q, "v"), in[0], in[1], in[2], 1.0f);
        glUniform1i(glGetUniformLocation(q, "mode"), mode);
        glClear(GL_COLOR_BUFFER_BIT);
        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
        glReadPixels(0, 0, 4, 4, GL_RGBA, GL_UNSIGNED_BYTE, out);
        int bad = 0;
        for (int k = 0; k < 3; k++) { int want = (int)(expect[mode][k] * 255.0f + 0.5f); if (out[k] > want + 2 || out[k] + 2 < want) bad = 1; }
        printf("GLES_COLOR %-10s got %3u %3u %3u want %3d %3d %3d %s\n", names[mode], out[0], out[1], out[2],
               (int)(expect[mode][0]*255+.5f), (int)(expect[mode][1]*255+.5f), (int)(expect[mode][2]*255+.5f), bad ? "BAD" : "ok");
        failures += bad;
    }
    // 5) Matrix uniforms (colour-management transforms are mat3/mat4 uniforms).
    GLuint mp = glCreateProgram();
    glAttachShader(mp, shader(GL_VERTEX_SHADER, "attribute vec2 pos; void main(){ gl_Position = vec4(pos,0.0,1.0); }"));
    glAttachShader(mp, shader(GL_FRAGMENT_SHADER,
        "precision highp float; uniform mat4 m4; uniform mat3 m3; uniform int which; uniform vec4 c;"
        " void main(){ gl_FragColor = which == 0 ? vec4((m4 * c).rgb, 1.0) : vec4(m3 * c.rgb, 1.0); }"));
    glBindAttribLocation(mp, 0, "pos");
    glLinkProgram(mp);
    glUseProgram(mp);
    // column-major: m4 swaps red/blue and halves green; m3 scales by 0.25/0.5/1.0
    const float m4[16] = {0,0,1,0, 0,0.5f,0,0, 1,0,0,0, 0,0,0,1};
    const float m3[9] = {0.25f,0,0, 0,0.5f,0, 0,0,1.0f};
    glUniformMatrix4fv(glGetUniformLocation(mp, "m4"), 1, GL_FALSE, m4);
    glUniformMatrix3fv(glGetUniformLocation(mp, "m3"), 1, GL_FALSE, m3);
    glUniform4f(glGetUniformLocation(mp, "c"), 0.8f, 0.6f, 0.2f, 1.0f);
    for (int which = 0; which < 2; which++) {
        glUniform1i(glGetUniformLocation(mp, "which"), which);
        glClear(GL_COLOR_BUFFER_BIT);
        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
        glReadPixels(0, 0, 4, 4, GL_RGBA, GL_UNSIGNED_BYTE, out);
        int want[2][3] = {{51, 77, 204}, {51, 77, 51}};
        int bad = 0; for (int k = 0; k < 3; k++) if (out[k] > want[which][k] + 2 || out[k] + 2 < want[which][k]) bad = 1;
        printf("GLES_MATRIX %s got %3u %3u %3u want %3d %3d %3d %s\n", which ? "mat3" : "mat4", out[0], out[1], out[2],
               want[which][0], want[which][1], want[which][2], bad ? "BAD" : "ok");
        failures += bad;
    }
    printf("GLES_PROBE_%s\n", failures ? "FAIL" : "PASS");
    return failures != 0;
}
