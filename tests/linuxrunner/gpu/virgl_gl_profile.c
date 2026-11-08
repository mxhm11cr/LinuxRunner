/* Actual guest GL capabilities, no version override and no software fallback
 * presented as hardware. Exit 2 means the requested Blender 4.x GL floor is
 * unavailable. Run on a disposable fixture with virtual renderD129 enabled. */
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GL/gl.h>
#include <fcntl.h>
#include <gbm.h>
#include <stdio.h>
#include <unistd.h>
int main(void) {
    int fd=open("/dev/dri/renderD129",O_RDWR|O_CLOEXEC);
    struct gbm_device *device=fd<0?NULL:gbm_create_device(fd);
    if(!device){puts("VIRGL_GL_PROFILE no hardware render node");return 1;}
    EGLDisplay display=eglGetPlatformDisplay(EGL_PLATFORM_GBM_KHR,device,NULL);
    EGLint major=0,minor=0;
    if(!eglInitialize(display,&major,&minor)||!eglBindAPI(EGL_OPENGL_API)) {
        printf("VIRGL_GL_PROFILE EGL initialization failed error=0x%x\n",eglGetError());return 1;
    }
    const EGLint configAttributes[]={EGL_RENDERABLE_TYPE,EGL_OPENGL_BIT,EGL_NONE};
    EGLConfig config;EGLint count=0;
    if(!eglChooseConfig(display,configAttributes,&config,1,&count)||!count)return 1;
    int supported43=0;
    const int versions[][2]={{4,3},{3,3}};
    for(unsigned i=0;i<2;i++) {
        const EGLint attributes[]={EGL_CONTEXT_MAJOR_VERSION,versions[i][0],EGL_CONTEXT_MINOR_VERSION,versions[i][1],EGL_CONTEXT_OPENGL_PROFILE_MASK,EGL_CONTEXT_OPENGL_CORE_PROFILE_BIT,EGL_NONE};
        EGLContext context=eglCreateContext(display,config,EGL_NO_CONTEXT,attributes);
        if(context==EGL_NO_CONTEXT) {
            printf("VIRGL_GL_PROFILE requested=%d.%d supported=0 egl_error=0x%x\n",versions[i][0],versions[i][1],eglGetError());continue;
        }
        if(eglMakeCurrent(display,EGL_NO_SURFACE,EGL_NO_SURFACE,context)) {
            printf("VIRGL_GL_PROFILE requested=%d.%d supported=1 renderer=%s version=%s glsl=%s\n",versions[i][0],versions[i][1],glGetString(GL_RENDERER),glGetString(GL_VERSION),glGetString(GL_SHADING_LANGUAGE_VERSION));
            if(i==0)supported43=1;
        }
        eglMakeCurrent(display,EGL_NO_SURFACE,EGL_NO_SURFACE,EGL_NO_CONTEXT);
        eglDestroyContext(display,context);
    }
    eglTerminate(display);gbm_device_destroy(device);close(fd);
    return supported43?0:2;
}
