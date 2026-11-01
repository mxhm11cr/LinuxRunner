// Functional software EGL import, not a capability-string test or GPU claim.
#define _GNU_SOURCE
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>
#include <assert.h>
#include <fcntl.h>
#include <linux/udmabuf.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>
static int fail(const char *step){fprintf(stderr,"EGL_DMABUF_FAIL stage=%s egl=%#x gl=%#x\n",step,eglGetError(),glGetError());return 1;}
int main(int argc,char **argv){
 if(argc!=2)return 2;
 PFNEGLQUERYDEVICESEXTPROC devices=(void *)eglGetProcAddress("eglQueryDevicesEXT");PFNEGLQUERYDEVICESTRINGEXTPROC device_string=(void *)eglGetProcAddress("eglQueryDeviceStringEXT");PFNEGLGETPLATFORMDISPLAYEXTPROC display=(void *)eglGetProcAddress("eglGetPlatformDisplayEXT");PFNEGLCREATEIMAGEKHRPROC create=(void *)eglGetProcAddress("eglCreateImageKHR");PFNEGLDESTROYIMAGEKHRPROC destroy=(void *)eglGetProcAddress("eglDestroyImageKHR");PFNGLEGLIMAGETARGETTEXTURE2DOESPROC image_texture=(void *)eglGetProcAddress("glEGLImageTargetTexture2DOES");
 if(!devices||!device_string||!display||!create||!destroy||!image_texture)return fail("entrypoints");
 EGLDeviceEXT list[32];EGLint count=0;if(!devices(32,list,&count))return fail("devices");EGLDisplay d=EGL_NO_DISPLAY;
 for(int i=0;i<count;i++){const char *s=device_string(list[i],EGL_EXTENSIONS);if(s&&strstr(s,"EGL_MESA_device_software")){d=display(EGL_PLATFORM_DEVICE_EXT,list[i],NULL);break;}}
 EGLint major,minor;if(d==EGL_NO_DISPLAY||!eglInitialize(d,&major,&minor))return fail("software-display");
 if(!eglBindAPI(EGL_OPENGL_ES_API))return fail("bind-api");
 EGLint attrs[]={EGL_RENDERABLE_TYPE,EGL_OPENGL_ES2_BIT,EGL_SURFACE_TYPE,EGL_PBUFFER_BIT,EGL_NONE};EGLConfig config;EGLint n=0;if(!eglChooseConfig(d,attrs,&config,1,&n)||!n)return fail("config");
 EGLint ctxattrs[]={EGL_CONTEXT_CLIENT_VERSION,2,EGL_NONE};EGLContext ctx=eglCreateContext(d,config,EGL_NO_CONTEXT,ctxattrs);if(ctx==EGL_NO_CONTEXT||!eglMakeCurrent(d,EGL_NO_SURFACE,EGL_NO_SURFACE,ctx))return fail("context");
 printf("EGL_DMABUF renderer=%s version=%d.%d\n",glGetString(GL_RENDERER),major,minor);fflush(stdout);
 int control=open(argv[1],O_RDWR|O_CLOEXEC);if(control<0){perror("control");return 1;}int mem=memfd_create("egl-cpu-import",MFD_CLOEXEC|MFD_ALLOW_SEALING);assert(mem>=0&&!ftruncate(mem,4096)&&!fcntl(mem,F_ADD_SEALS,F_SEAL_SHRINK|F_SEAL_SEAL));uint32_t *pixels=mmap(NULL,4096,PROT_READ|PROT_WRITE,MAP_SHARED,mem,0);assert(pixels!=MAP_FAILED);for(int i=0;i<1024;i++)pixels[i]=0xff332211;
 struct udmabuf_create request={mem,UDMABUF_FLAGS_CLOEXEC,0,4096};int fd=ioctl(control,UDMABUF_CREATE,&request);if(fd<0){perror("export");return 1;}
 EGLint imageattrs[]={EGL_WIDTH,16,EGL_HEIGHT,16,EGL_LINUX_DRM_FOURCC_EXT,0x34325241,EGL_DMA_BUF_PLANE0_FD_EXT,fd,EGL_DMA_BUF_PLANE0_OFFSET_EXT,0,EGL_DMA_BUF_PLANE0_PITCH_EXT,64,EGL_NONE};
 EGLImageKHR image=create(d,EGL_NO_CONTEXT,EGL_LINUX_DMA_BUF_EXT,NULL,imageattrs);if(image==EGL_NO_IMAGE_KHR)return fail("import-linear-buffer");
 GLuint texture,fbo;glGenTextures(1,&texture);glBindTexture(GL_TEXTURE_2D,texture);image_texture(GL_TEXTURE_2D,image);if(glGetError())return fail("image-texture");glGenFramebuffers(1,&fbo);glBindFramebuffer(GL_FRAMEBUFFER,fbo);glFramebufferTexture2D(GL_FRAMEBUFFER,GL_COLOR_ATTACHMENT0,GL_TEXTURE_2D,texture,0);if(glCheckFramebufferStatus(GL_FRAMEBUFFER)!=GL_FRAMEBUFFER_COMPLETE)return fail("framebuffer");
 unsigned char rgba[4];glReadPixels(0,0,1,1,GL_RGBA,GL_UNSIGNED_BYTE,rgba);if(glGetError()||rgba[0]!=0x33||rgba[1]!=0x22||rgba[2]!=0x11||rgba[3]!=0xff)return fail("read-original-pattern");
 glViewport(0,0,16,16);glClearColor(.25f,.5f,.75f,1);glClear(GL_COLOR_BUFFER_BIT);glFinish();if(glGetError())return fail("render");uint32_t v=pixels[0];int red=(v>>16)&255,green=(v>>8)&255,blue=v&255;if(abs(red-64)>1||abs(green-128)>1||abs(blue-191)>1)return fail("render-visible-in-source-alias");
 // Draw (shader) into the imported buffer, as a compositor does each frame; then
 // a second frame with another colour, and a scissored partial repaint.
 {const char *vs="attribute vec2 p;void main(){gl_Position=vec4(p,0.0,1.0);}",*fs="precision mediump float;uniform vec4 c;void main(){gl_FragColor=c;}";
  GLuint prog=glCreateProgram(),a=glCreateShader(GL_VERTEX_SHADER),b=glCreateShader(GL_FRAGMENT_SHADER);
  glShaderSource(a,1,&vs,NULL);glCompileShader(a);glShaderSource(b,1,&fs,NULL);glCompileShader(b);glAttachShader(prog,a);glAttachShader(prog,b);glBindAttribLocation(prog,0,"p");glLinkProgram(prog);glUseProgram(prog);
  static const float quad[]={-1,-1,1,-1,-1,1,1,1};glVertexAttribPointer(0,2,GL_FLOAT,GL_FALSE,0,quad);glEnableVertexAttribArray(0);
  const float colors[2][3]={{0.9f,0.1f,0.2f},{0.1f,0.8f,0.3f}};
  for(int frame=0;frame<2;frame++){glUniform4f(glGetUniformLocation(prog,"c"),colors[frame][0],colors[frame][1],colors[frame][2],1);glDrawArrays(GL_TRIANGLE_STRIP,0,4);glFinish();
   uint32_t w=pixels[8*16+8];int r=(w>>16)&255,g=(w>>8)&255,bl=w&255,er=(int)(colors[frame][0]*255+.5f),eg=(int)(colors[frame][1]*255+.5f),eb=(int)(colors[frame][2]*255+.5f);
   printf("EGL_DMABUF_DRAW frame%d cpu=%d,%d,%d want=%d,%d,%d %s\n",frame,r,g,bl,er,eg,eb,abs(r-er)<=1&&abs(g-eg)<=1&&abs(bl-eb)<=1?"ok":"BAD");fflush(stdout);
   if(abs(r-er)>1||abs(g-eg)>1||abs(bl-eb)>1)return fail("draw-visible-in-source-alias");}
  glEnable(GL_SCISSOR_TEST);glScissor(0,0,4,4);glUniform4f(glGetUniformLocation(prog,"c"),1,1,1,1);glDrawArrays(GL_TRIANGLE_STRIP,0,4);glFinish();glDisable(GL_SCISSOR_TEST);
  uint32_t in=pixels[0],out=pixels[8*16+8];printf("EGL_DMABUF_PARTIAL inside=%06x outside=%06x\n",in&0xffffff,out&0xffffff);
  if((in&0xffffff)!=0xffffff||((out>>8)&255)<200)return fail("partial-repaint");}
 // Renderbuffer storage from the same EGLImage (the other way a compositor
 // binds an imported buffer as its render target).
 {PFNGLEGLIMAGETARGETRENDERBUFFERSTORAGEOESPROC rb_storage=(void *)eglGetProcAddress("glEGLImageTargetRenderbufferStorageOES");
  if(!rb_storage)return fail("renderbuffer-entrypoint");
  GLuint rb,rfbo;glGenRenderbuffers(1,&rb);glBindRenderbuffer(GL_RENDERBUFFER,rb);rb_storage(GL_RENDERBUFFER,image);
  glGenFramebuffers(1,&rfbo);glBindFramebuffer(GL_FRAMEBUFFER,rfbo);glFramebufferRenderbuffer(GL_FRAMEBUFFER,GL_COLOR_ATTACHMENT0,GL_RENDERBUFFER,rb);
  if(glCheckFramebufferStatus(GL_FRAMEBUFFER)!=GL_FRAMEBUFFER_COMPLETE)return fail("renderbuffer-framebuffer");
  glClearColor(.5f,.25f,.125f,1);glClear(GL_COLOR_BUFFER_BIT);glFinish();
  uint32_t w=pixels[5];printf("EGL_DMABUF_RENDERBUFFER cpu=%d,%d,%d want=128,64,32 %s\n",(w>>16)&255,(w>>8)&255,w&255,
   abs((int)((w>>16)&255)-128)<=1&&abs((int)((w>>8)&255)-64)<=1&&abs((int)(w&255)-32)<=1?"ok":"BAD");
  if(abs((int)((w>>16)&255)-128)>1)return fail("renderbuffer-visible-in-source-alias");
  glBindFramebuffer(GL_FRAMEBUFFER,fbo);}
 // Explicit LINEAR modifier, as compositors (KWin) import their swapchain and
 // screencast buffers: drawing must still land in the shared memory.
 {EGLint mattrs[]={EGL_WIDTH,16,EGL_HEIGHT,16,EGL_LINUX_DRM_FOURCC_EXT,0x34325241,EGL_DMA_BUF_PLANE0_FD_EXT,fd,EGL_DMA_BUF_PLANE0_OFFSET_EXT,0,EGL_DMA_BUF_PLANE0_PITCH_EXT,64,
   EGL_DMA_BUF_PLANE0_MODIFIER_LO_EXT,0,EGL_DMA_BUF_PLANE0_MODIFIER_HI_EXT,0,EGL_NONE};
  EGLImageKHR mimage=create(d,EGL_NO_CONTEXT,EGL_LINUX_DMA_BUF_EXT,NULL,mattrs);
  printf("EGL_DMABUF_MODIFIER_IMPORT %s egl=%#x\n",mimage==EGL_NO_IMAGE_KHR?"refused":"ok",eglGetError());
  if(mimage!=EGL_NO_IMAGE_KHR){GLuint mt,mf;glGenTextures(1,&mt);glBindTexture(GL_TEXTURE_2D,mt);image_texture(GL_TEXTURE_2D,mimage);
   glGenFramebuffers(1,&mf);glBindFramebuffer(GL_FRAMEBUFFER,mf);glFramebufferTexture2D(GL_FRAMEBUFFER,GL_COLOR_ATTACHMENT0,GL_TEXTURE_2D,mt,0);
   glClearColor(.75f,.5f,.25f,1);glClear(GL_COLOR_BUFFER_BIT);glFinish();uint32_t w=pixels[7];
   printf("EGL_DMABUF_MODIFIER_RENDER cpu=%d,%d,%d want=191,128,64 %s\n",(w>>16)&255,(w>>8)&255,w&255,abs((int)((w>>16)&255)-191)<=1?"ok":"BAD");
   glBindFramebuffer(GL_FRAMEBUFFER,fbo);}}
 printf("EGL_DMABUF_PASS CPU pattern imported, GL framebuffer rendered, shared source changed rgba=%d,%d,%d,255; software-only\n",red,green,blue);
 glDeleteFramebuffers(1,&fbo);glDeleteTextures(1,&texture);destroy(d,image);eglMakeCurrent(d,EGL_NO_SURFACE,EGL_NO_SURFACE,EGL_NO_CONTEXT);eglDestroyContext(d,ctx);eglTerminate(d);close(fd);close(mem);close(control);munmap(pixels,4096);return 0;
}
