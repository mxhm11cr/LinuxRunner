#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <assert.h>
#include <stdio.h>
#include <string.h>
int main(void){
 PFNEGLQUERYDEVICESEXTPROC devices=(void *)eglGetProcAddress("eglQueryDevicesEXT");PFNEGLQUERYDEVICESTRINGEXTPROC device_string=(void *)eglGetProcAddress("eglQueryDeviceStringEXT");PFNEGLGETPLATFORMDISPLAYEXTPROC display=(void *)eglGetProcAddress("eglGetPlatformDisplayEXT");
 if(!devices||!device_string||!display){puts("EGL_SOFTWARE_BLOCKED missing-device-query");return 77;}EGLDeviceEXT list[32];EGLint count=0;if(!devices(32,list,&count)){printf("EGL_SOFTWARE_BLOCKED query-error=%x\n",eglGetError());return 77;}
 for(int i=0;i<count;i++){
  const char *extensions=device_string(list[i],EGL_EXTENSIONS);if(!extensions||!strstr(extensions,"EGL_MESA_device_software"))continue;
  EGLDisplay d=display(EGL_PLATFORM_DEVICE_EXT,list[i],NULL);EGLint major,minor;if(!eglInitialize(d,&major,&minor)){printf("EGL_SOFTWARE_BLOCKED initialize=%x\n",eglGetError());return 77;}
  const char *caps=eglQueryString(d,EGL_EXTENSIONS);printf("EGL_SOFTWARE version=%d.%d vendor=%s\n",major,minor,eglQueryString(d,EGL_VENDOR));printf("EGL_SOFTWARE_EXTENSIONS %s\n",caps?:"");
  PFNEGLQUERYDMABUFFORMATSEXTPROC formats=(void *)eglGetProcAddress("eglQueryDmaBufFormatsEXT");EGLint n=0;EGLBoolean query=formats?formats(d,0,NULL,&n):EGL_FALSE;printf("EGL_DMA_BUF formats_query=%d formats=%d error=%x\n",query,n,eglGetError());eglTerminate(d);return 0;
 }puts("EGL_SOFTWARE_BLOCKED no-software-device");return 77;
}
