/* Opt-in Mesa/Metal interoperability. Mesa owns genuine core contexts and GL
 * semantics; eligible draws use the GPU device. Unsupported draws explicitly
 * retain Mesa. No version/vendor/renderer strings are replaced. */
#define _GNU_SOURCE
#define GL_GLEXT_PROTOTYPES
#define LR_GPU_GUEST 1
#include "guest_gpu.h"
#include <GL/gl.h>
#include <GL/glext.h>
#include <GL/glx.h>
#include <GL/glxext.h>
#include <EGL/egl.h>
#include <dlfcn.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>
#include <time.h>
#include <stdint.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/syscall.h>
#include <errno.h>
#include <math.h>
#include <zlib.h>
#define CONTEXTS 32
#define CAPTURES 256
static _Atomic uint64_t capture_epoch=1;   // bumped whenever the capture table changes (program memo validity)
enum {REPORT_UPLOAD,REPORT_WRITEBACK,REPORT_WRITEBACK_WINDOW,REPORT_WB_READ,REPORT_GPU_DRAW,REPORT_MESA_DRAW,REPORT_P_STATE,REPORT_P_TARGETS,REPORT_P_PROGRAM,REPORT_P_UNIFORMS,REPORT_P_VERTICES,REPORT_P_STATE_IOCTL,REPORT_P_DRAW_IOCTL,REPORT_P_FF,REPORT_P_LOOKUP,REPORT_P_INDICES,REPORT_P_U_SAMPLERS,REPORT_P_U_BLOCKS,REPORT_SLOTS};
static _Atomic int report_phases;static uint64_t report_now(void);static void report_add(unsigned slot,uint64_t started,uint64_t bytes);
// Hot-path costs that matter under the interpreter: the interposer is loaded at
// startup (LD_PRELOAD), so initial-exec TLS is valid and avoids __tls_get_addr;
// the thread id and configuration are read once instead of per GL call.
#define LR_TLS __thread __attribute__((tls_model("initial-exec")))
static LR_TLS long cached_tid;
static void forget_tid(void){cached_tid=0;}   // fork child: new thread id
static void register_tid_reset(void){pthread_atfork(NULL,NULL,forget_tid);}
static long current_tid(void){if(!cached_tid){static pthread_once_t once=PTHREAD_ONCE_INIT;pthread_once(&once,register_tid_reset);cached_tid=syscall(SYS_gettid);}return cached_tid;}
// LR_MESA_METAL_* switches are process configuration: read each once.
static const char *config(const char *name){
 static struct {_Atomic(const char *) name;const char *value;_Atomic int ready;} cache[16];
 for(unsigned i=0;i<16;i++){const char *key=atomic_load(&cache[i].name);
  if(key&&!strcmp(key,name)){if(atomic_load(&cache[i].ready))return cache[i].value;break;}
  if(!key){const char *expected=NULL;if(atomic_compare_exchange_strong(&cache[i].name,&expected,name)){cache[i].value=getenv(name);atomic_store(&cache[i].ready,1);return cache[i].value;}
   if(!strcmp(expected,name)){if(atomic_load(&cache[i].ready))return cache[i].value;break;}}}
 return getenv(name);
}
// Window size of the current drawable. glXQueryDrawable can be an X round trip;
// the size is cached per drawable and re-read after every buffer swap and
// MakeCurrent (when GL itself picks up a resized window).
static LR_TLS unsigned long window_cached;static LR_TLS unsigned window_cached_w,window_cached_h;static LR_TLS uint64_t window_cached_epoch;
static _Atomic uint64_t window_epoch=1;
// EGL clients (Wayland, GBM, surfaceless) are tracked like GLX ones: the
// context and draw surface bound by eglMakeCurrent are kept per thread, and GLX
// is consulted only while no EGL context is bound on this thread.
static LR_TLS void *egl_context;static LR_TLS EGLDisplay egl_display;static LR_TLS EGLSurface egl_draw;
static void *original(const char *);
static void window_size(unsigned *w,unsigned *h){
 if(egl_context){uint64_t e=atomic_load_explicit(&window_epoch,memory_order_relaxed);unsigned long d=(unsigned long)(uintptr_t)egl_draw;
  if(d&&d==window_cached&&e==window_cached_epoch){*w=window_cached_w;*h=window_cached_h;return;}
  EGLint ww=0,hh=0;typeof(&eglQuerySurface) query=original("eglQuerySurface");
  if(egl_draw!=EGL_NO_SURFACE&&query){query(egl_display,egl_draw,EGL_WIDTH,&ww);query(egl_display,egl_draw,EGL_HEIGHT,&hh);}   // surfaceless: no window framebuffer
  window_cached=d;window_cached_w=(unsigned)ww;window_cached_h=(unsigned)hh;window_cached_epoch=e;*w=(unsigned)ww;*h=(unsigned)hh;return;}
 GLXDrawable d=glXGetCurrentDrawable();uint64_t e=atomic_load_explicit(&window_epoch,memory_order_relaxed);
 if(d&&d==window_cached&&e==window_cached_epoch){*w=window_cached_w;*h=window_cached_h;return;}
 unsigned ww=0,hh=0;glXQueryDrawable(glXGetCurrentDisplay(),d,GLX_WIDTH,&ww);glXQueryDrawable(glXGetCurrentDisplay(),d,GLX_HEIGHT,&hh);
 window_cached=d;window_cached_w=ww;window_cached_h=hh;window_cached_epoch=e;*w=ww;*h=hh;
}
static void window_size_changed(void){atomic_fetch_add_explicit(&window_epoch,1,memory_order_relaxed);}
#define SOURCE_BUDGET (8u*1024u*1024u)
struct capture {uintptr_t group;GLuint name;uint64_t generation;char *source[2];uint32_t packed_bytes[2],raw_bytes[2];};
struct gpu_program {GLuint mesa;uint64_t generation,layout;int valid;GLint location[LR_GL_MAX_UNIFORMS];unsigned char located[LR_GL_MAX_UNIFORMS];   /* glGetUniformLocation results: fixed for a link (a relink is a new generation) */
 uint8_t gl_location[LR_GL_MAX_ATTRIBUTES];char shader_label[128];struct lr_gl_link link;struct lr_gl_uniform_extents extents;struct lr_gl_blocks *blocks;struct lr_gl_storages storages;};
struct color_target {GLenum buffer,format,type;unsigned channels,bytes;size_t offset,size;};
struct rejection {GLuint program;const char *reason;GLenum mode;unsigned kind,depth;GLint base_vertex;};
struct program_memo {GLuint mesa;unsigned slot;uint64_t layout,epoch,generation;};
struct window_targets {int valid;GLint buffer,second;unsigned srgb,width,height,count,framebuffer_srgb;size_t color_bytes;struct color_target target;};   // the window framebuffer's format is fixed for a context
struct bridge_context {struct program_memo memo[16];struct window_targets window_targets;void *handle;GLenum error;uintptr_t group;uint64_t id;long owner;int deleted,fd,es,es_known;unsigned width,height;GLuint draw_framebuffer,last_program;char last_program_label[128],last_shader_label[128];GLenum pixel_format,pixel_type;unsigned pixel_bytes,channels,framebuffer_srgb,target_count,budget_noted;struct color_target targets[4];size_t color_bytes;struct gpu_program program[LR_GL_MAX_PROGRAMS];unsigned next;unsigned char *before,*after,*oracle;float *depth_before,*depth_after,*depth_oracle;size_t depth_capacity;unsigned depth_test,depth_bits,stencil_test;unsigned char *stencil_before,*stencil_after,*stencil_oracle;size_t stencil_capacity;size_t pixel_capacity;GLuint transfer_texture,transfer_fbo;uint64_t attempted,committed,verified,mismatched,fallback;struct rejection rejection[128];unsigned rejections;GLuint traced_program[128];unsigned traced;struct resident_state *resident;uint64_t stats_gpu_ns,stats_submissions;};
// GPU-resident textures (mesa_metal_resident.inc).
struct resident_targets;
static LR_TLS int resident_reentry;
static _Atomic uint64_t stats_frames;static _Atomic uint64_t stats_blits;static void stats_write(int force);
static void report_reason(const char *reason);static void report_reasonf(const char *format,...) __attribute__((format(printf,1,2)));
static int resident_enabled(void);
static void resident_sync(struct bridge_context *c,GLuint only_name,int only);
static void resident_unbind(struct bridge_context *c);
static int resident_is_bound(struct bridge_context *c);
static struct capture captures[CAPTURES];static size_t source_bytes;
static struct bridge_context contexts[CONTEXTS];static uintptr_t next_group=1;static uint64_t next_generation=1,next_context_id=1;
static pthread_mutex_t records=PTHREAD_MUTEX_INITIALIZER;static LR_TLS struct bridge_context *local_context;
static atomic_uint log_budget=192;
static void note(const char *format,...){unsigned old=atomic_load(&log_budget);while(old&&!atomic_compare_exchange_weak(&log_budget,&old,old-1)){}if(!old)return;va_list args;va_start(args,format);flockfile(stderr);fprintf(stderr,"MESA_METAL pid=%ld ",(long)getpid());vfprintf(stderr,format,args);fputc('\n',stderr);funlockfile(stderr);va_end(args);}
static void *(*system_dlsym)(void *,const char *);static pthread_once_t resolver_once=PTHREAD_ONCE_INIT;
static void initialize_resolver(void){system_dlsym=dlvsym(RTLD_NEXT,"dlsym","GLIBC_2.17");}
static void *lookup(void *handle,const char *name){pthread_once(&resolver_once,initialize_resolver);return system_dlsym?system_dlsym(handle,name):NULL;}
static void *original(const char *);
static void report_reasonf(const char *format,...) __attribute__((format(printf,1,2)));
// A refused device request is a reason too (it otherwise ends a draw silently).
static int call_gpu(struct bridge_context *c,struct lr_gpu_request *r){r->version=LR_GPU_VERSION;if(ioctl(c->fd,LR_GPU_IOCTL,r)==0)return 1;report_reasonf("ioctl:op%u-errno%d",r->operation,errno);return 0;}
static struct bridge_context *find_context(void *h){for(unsigned i=0;i<CONTEXTS;i++)if(contexts[i].handle==h&&h)return &contexts[i];return NULL;}
static struct bridge_context *register_context(void *h,void *shared){if(!h)return NULL;struct bridge_context *c=find_context(h);if(c)return c;struct bridge_context *s=find_context(shared);for(unsigned i=0;i<CONTEXTS;i++)if(!contexts[i].handle){contexts[i]=(struct bridge_context){.handle=h,.group=s?s->group:next_group++,.id=next_context_id++,.fd=-1};return &contexts[i];}return NULL;}
static void drop_context(struct bridge_context *c){
 if(c->fd>=0)close(c->fd);for(unsigned i=0;i<LR_GL_MAX_PROGRAMS;i++)free(c->program[i].blocks);free(c->before);free(c->after);free(c->oracle);free(c->depth_before);free(c->depth_after);free(c->depth_oracle);free(c->stencil_before);free(c->stencil_after);free(c->stencil_oracle);
 note("context_release context_id=%llu program=%u program_label=%s shader_label=%s gpu_draws=%llu verified=%llu mismatch=%llu software_draws=%llu",(unsigned long long)c->id,c->last_program,c->last_program_label,c->last_shader_label,(unsigned long long)c->committed,(unsigned long long)c->verified,(unsigned long long)c->mismatched,(unsigned long long)c->fallback);
 uintptr_t group=c->group;free(c->resident);memset(c,0,sizeof *c);int used=0;for(unsigned i=0;i<CONTEXTS;i++)if(contexts[i].handle&&contexts[i].group==group)used=1;
 if(!used)for(unsigned i=0;i<CAPTURES;i++)if(captures[i].group==group){for(unsigned j=0;j<2;j++)if(captures[i].source[j]){source_bytes-=captures[i].packed_bytes[j];free(captures[i].source[j]);}memset(&captures[i],0,sizeof captures[i]);atomic_fetch_add(&capture_epoch,1);}
}
static struct bridge_context *current_context(void){void *h=egl_context?egl_context:(void *)glXGetCurrentContext();if(local_context&&local_context->handle==h)return local_context;pthread_mutex_lock(&records);struct bridge_context *c=register_context(h,NULL);if(c)c->owner=current_tid();pthread_mutex_unlock(&records);local_context=c;return c;}
// OpenGL ES contexts (EGL clients): desktop-only state does not exist there.
// Queries the interposer makes are answered with ES semantics instead of
// raising GL errors the application would see; ES-only replacements are used
// for entry points desktop GL has and ES lacks.
static int es_current(void){struct bridge_context *c=local_context;if(!c)return 0;if(!c->es_known){const char *v=(const char *)glGetString(GL_VERSION);c->es=v&&!strncmp(v,"OpenGL ES",9);c->es_known=1;}return c->es;}
static int es_extension(const char *name){const char *e=(const char *)glGetString(GL_EXTENSIONS);size_t n=strlen(name);for(const char *p=e;p&&(p=strstr(p,name));p+=n)if((p==e||p[-1]==' ')&&(p[n]==' '||!p[n]))return 1;return 0;}
static GLboolean state_enabled(GLenum cap){
 if(es_current())switch(cap){
  case GL_COLOR_LOGIC_OP:case GL_LINE_SMOOTH:return GL_FALSE;
  case GL_FRAMEBUFFER_SRGB:if(!es_extension("GL_EXT_sRGB_write_control"))return GL_TRUE;break;   // ES: sRGB images always encode
 }
 return glIsEnabled(cap);
}
static void state_draw_buffer(GLenum buffer){
 if(!es_current()){glDrawBuffer(buffer);return;}
 GLenum list[16]={0};unsigned n=1;if(buffer>=GL_COLOR_ATTACHMENT0&&buffer<=GL_COLOR_ATTACHMENT15){n=buffer-GL_COLOR_ATTACHMENT0+1;for(unsigned i=0;i<n;i++)list[i]=GL_NONE;}   // ES: attachment i at position i
 list[n-1]=buffer;glDrawBuffers((GLsizei)n,list);
}
static void invalid_value(void){struct bridge_context *c=current_context();if(c&&!c->error)c->error=GL_INVALID_VALUE;}
static void release_transfer(void){if(!local_context)return;if(local_context->transfer_texture){glDeleteTextures(1,&local_context->transfer_texture);local_context->transfer_texture=0;}if(local_context->transfer_fbo){glDeleteFramebuffers(1,&local_context->transfer_fbo);local_context->transfer_fbo=0;}}
static void binding_changed(void *h){pthread_mutex_lock(&records);struct bridge_context *old=local_context,*now=register_context(h,NULL);if(old&&old!=now){old->owner=0;if(old->deleted)drop_context(old);}if(now)now->owner=current_tid();local_context=now;pthread_mutex_unlock(&records);}
GLXContext glXCreateContextAttribsARB(Display *d,GLXFBConfig f,GLXContext shared,Bool direct,const int *attributes){typeof(&glXCreateContextAttribsARB) fn=original("glXCreateContextAttribsARB");GLXContext h=fn?fn(d,f,shared,direct,attributes):NULL;pthread_mutex_lock(&records);register_context(h,shared);pthread_mutex_unlock(&records);note("GLX_core_context success=%d shared=%d provider=Mesa requested_attributes_preserved=1",h!=NULL,shared!=NULL);return h;}
GLXContext glXCreateContext(Display *d,XVisualInfo *v,GLXContext shared,Bool direct){typeof(&glXCreateContext) fn=original("glXCreateContext");GLXContext h=fn?fn(d,v,shared,direct):NULL;pthread_mutex_lock(&records);register_context(h,shared);pthread_mutex_unlock(&records);return h;}
GLXContext glXCreateNewContext(Display *d,GLXFBConfig f,int type,GLXContext shared,Bool direct){typeof(&glXCreateNewContext) fn=original("glXCreateNewContext");GLXContext h=fn?fn(d,f,type,shared,direct):NULL;pthread_mutex_lock(&records);register_context(h,shared);pthread_mutex_unlock(&records);return h;}
Bool glXMakeCurrent(Display *d,GLXDrawable w,GLXContext h){window_size_changed();typeof(&glXMakeCurrent) fn=original("glXMakeCurrent");if(local_context&&local_context->handle!=h){resident_sync(local_context,0,0);resident_unbind(local_context);release_transfer();}Bool r=fn?fn(d,w,h):False;if(r)binding_changed(h);return r;}
Bool glXMakeContextCurrent(Display *d,GLXDrawable w,GLXDrawable read,GLXContext h){window_size_changed();typeof(&glXMakeContextCurrent) fn=original("glXMakeContextCurrent");if(local_context&&local_context->handle!=h){resident_sync(local_context,0,0);resident_unbind(local_context);release_transfer();}Bool r=fn?fn(d,w,read,h):False;if(r)binding_changed(h);return r;}
static void stats_write(int force);
void glXDestroyContext(Display *d,GLXContext h){stats_write(1);typeof(&glXDestroyContext) fn=original("glXDestroyContext");if(local_context&&local_context->handle==h)release_transfer();if(fn)fn(d,h);pthread_mutex_lock(&records);struct bridge_context *c=find_context(h);if(c){c->deleted=1;if(!c->owner)drop_context(c);}pthread_mutex_unlock(&records);}
EGLContext eglCreateContext(EGLDisplay d,EGLConfig f,EGLContext shared,const EGLint *attributes){typeof(&eglCreateContext) fn=original("eglCreateContext");EGLContext h=fn?fn(d,f,shared,attributes):EGL_NO_CONTEXT;pthread_mutex_lock(&records);register_context(h,shared);pthread_mutex_unlock(&records);note("EGL_context success=%d shared=%d provider=Mesa requested_attributes_preserved=1",h!=EGL_NO_CONTEXT,shared!=EGL_NO_CONTEXT);return h;}
EGLBoolean eglMakeCurrent(EGLDisplay d,EGLSurface draw,EGLSurface read,EGLContext h){window_size_changed();typeof(&eglMakeCurrent) fn=original("eglMakeCurrent");if(local_context&&local_context->handle!=h){resident_sync(local_context,0,0);resident_unbind(local_context);release_transfer();}EGLBoolean r=fn?fn(d,draw,read,h):EGL_FALSE;if(r){egl_context=h;egl_display=d;egl_draw=h?draw:EGL_NO_SURFACE;binding_changed(h);}return r;}
EGLBoolean eglDestroyContext(EGLDisplay d,EGLContext h){stats_write(1);typeof(&eglDestroyContext) fn=original("eglDestroyContext");if(local_context&&local_context->handle==h)release_transfer();EGLBoolean r=fn?fn(d,h):EGL_FALSE;pthread_mutex_lock(&records);struct bridge_context *c=find_context(h);if(c){c->deleted=1;if(!c->owner)drop_context(c);}pthread_mutex_unlock(&records);return r;}
static void invalidate_capture(struct bridge_context *c,GLuint program){
 pthread_mutex_lock(&records);for(unsigned i=0;i<CAPTURES;i++)if(captures[i].group==c->group&&captures[i].name==program){for(unsigned j=0;j<2;j++){source_bytes-=captures[i].packed_bytes[j];free(captures[i].source[j]);}memset(&captures[i],0,sizeof captures[i]);atomic_fetch_add(&capture_epoch,1);break;}pthread_mutex_unlock(&records);
}
static int capture_program_sources(struct bridge_context *c,GLuint program){
 int stored=0;const char *reason="unknown",*stage_name="unknown";GLuint attached[3]={0};GLsizei count=0;glGetAttachedShaders(program,3,&count,attached);if(count!=2){reason="attached-shader-count";goto refuse;}
 char *sources[2]={0},*packed[2]={0};uint32_t packed_sizes[2]={0},raw_sizes[2]={0};for(unsigned i=0;i<(unsigned)count;i++){GLint stage=0,length=0;glGetShaderiv(attached[i],GL_SHADER_TYPE,&stage);glGetShaderiv(attached[i],GL_SHADER_SOURCE_LENGTH,&length);int index=stage==GL_VERTEX_SHADER?0:stage==GL_FRAGMENT_SHADER?1:-1;stage_name=stage==GL_VERTEX_SHADER?"vertex":stage==GL_FRAGMENT_SHADER?"fragment":"other";if(index<0){reason="unsupported-stage";goto done;}if(length<=1||length>LR_GL_MAX_SOURCE){reason="source-length";goto done;}if(sources[index]){reason="duplicate-stage";goto done;}sources[index]=calloc(length,1);if(!sources[index]){reason="source-allocation";goto done;}glGetShaderSource(attached[i],length,NULL,sources[index]);}
 if(!sources[0]||!sources[1]){reason="missing-vertex-or-fragment";goto done;}for(unsigned i=0;i<2;i++){raw_sizes[i]=strlen(sources[i])+1;uLongf capacity=compressBound(raw_sizes[i]);packed[i]=malloc(capacity);if(!packed[i]){reason="compressed-allocation";goto done;}if(compress2((Bytef *)packed[i],&capacity,(const Bytef *)sources[i],raw_sizes[i],Z_BEST_SPEED)!=Z_OK){reason="source-compression";goto done;}packed_sizes[i]=capacity;}
 pthread_mutex_lock(&records);struct capture *p=NULL;for(unsigned i=0;i<CAPTURES;i++)if(captures[i].group==c->group&&captures[i].name==program){p=&captures[i];break;}if(!p)for(unsigned i=0;i<CAPTURES;i++)if(!captures[i].group){p=&captures[i];break;}
 if(!p)reason="capture-table-full";else{size_t old=0,bytes=packed_sizes[0]+packed_sizes[1];for(unsigned i=0;i<2;i++)if(p->source[i])old+=p->packed_bytes[i];if(bytes>SOURCE_BUDGET-(source_bytes-old))reason="source-budget";else{for(unsigned i=0;i<2;i++)free(p->source[i]);*p=(struct capture){.group=c->group,.name=program,.generation=next_generation++,.source={packed[0],packed[1]},.packed_bytes={packed_sizes[0],packed_sizes[1]},.raw_bytes={raw_sizes[0],raw_sizes[1]}};source_bytes=source_bytes-old+bytes;packed[0]=packed[1]=NULL;stored=1;atomic_fetch_add(&capture_epoch,1);}}
 pthread_mutex_unlock(&records);
 done:if(!stored){char label[128]={0};glGetObjectLabel(GL_PROGRAM,program,sizeof label,NULL,label);note("capture_refusal program=%u label=%s reason=%s stage=%s attached=%d",program,label,reason,stage_name,(int)count);}free(sources[0]);free(sources[1]);free(packed[0]);free(packed[1]);return stored;
 refuse:{char label[128]={0};glGetObjectLabel(GL_PROGRAM,program,sizeof label,NULL,label);note("capture_refusal program=%u label=%s reason=%s stage=%s attached=%d",program,label,reason,stage_name,(int)count);return 0;}
}
void glLinkProgram(GLuint program){
 typeof(&glLinkProgram) fn=original("glLinkProgram");if(fn)fn(program);GLint linked=0;glGetProgramiv(program,GL_LINK_STATUS,&linked);if(!linked)return;
 struct bridge_context *c=current_context();if(c&&!capture_program_sources(c,program))invalidate_capture(c,program);
}
static void delete_gpu_program(struct bridge_context *c,unsigned slot){if(c->program[slot].valid){struct lr_gpu_request r={.operation=LR_GPU_DELETE_PROGRAM,.count=slot+1};call_gpu(c,&r);}free(c->program[slot].blocks);memset(&c->program[slot],0,sizeof c->program[slot]);}
static _Atomic uint64_t stats_program_hits,stats_program_compiles,stats_program_failures;
static struct gpu_program *compile_program(struct bridge_context *c,GLuint mesa,unsigned *slot){
 char *source[2]={0};uint64_t generation=0,layout=c->target_count;for(unsigned i=0;i<c->target_count;i++)layout=(layout*65599)^c->targets[i].format;
 // Per-draw fast path: same program, target layout and capture table as a recent hit.
 uint64_t epoch=atomic_load_explicit(&capture_epoch,memory_order_acquire);struct program_memo *m=&c->memo[mesa%16];
 if(m->mesa==mesa&&m->epoch==epoch&&m->layout==layout){struct gpu_program *q=&c->program[m->slot];if(q->mesa==mesa&&q->generation==m->generation&&q->layout==layout){*slot=m->slot;atomic_fetch_add(&stats_program_hits,1);return q->valid>0?q:NULL;}}
 pthread_mutex_lock(&records);for(unsigned i=0;i<CAPTURES;i++)if(captures[i].group==c->group&&captures[i].name==mesa){generation=captures[i].generation;for(unsigned k=0;k<LR_GL_MAX_PROGRAMS;k++)if(c->program[k].mesa==mesa&&c->program[k].generation==generation&&c->program[k].layout==layout){pthread_mutex_unlock(&records);*slot=k;*m=(struct program_memo){.mesa=mesa,.slot=k,.layout=layout,.epoch=epoch,.generation=generation};atomic_fetch_add(&stats_program_hits,1);return c->program[k].valid>0?&c->program[k]:NULL;}for(unsigned j=0;j<2;j++){uLongf bytes=captures[i].raw_bytes[j];source[j]=calloc(bytes,1);if(source[j]&&uncompress((Bytef *)source[j],&bytes,(const Bytef *)captures[i].source[j],captures[i].packed_bytes[j])!=Z_OK){free(source[j]);source[j]=NULL;}}break;}pthread_mutex_unlock(&records);if(!source[0]||!source[1]){note("program_fallback program=%u reason=%s stage=vertex+fragment",mesa,generation?"source-decompression":"source-capture-unavailable");goto failure;}
 for(unsigned i=0;i<LR_GL_MAX_PROGRAMS;i++)if(c->program[i].mesa==mesa&&c->program[i].generation==generation&&c->program[i].layout==layout){free(source[0]);free(source[1]);*slot=i;atomic_fetch_add(&stats_program_hits,1);return c->program[i].valid>0?&c->program[i]:NULL;}
 *slot=c->next++%LR_GL_MAX_PROGRAMS;delete_gpu_program(c,*slot);struct gpu_program *p=&c->program[*slot];p->mesa=mesa;p->generation=generation;p->layout=layout;GLuint attached[3]={0};GLsizei attached_count=0;glGetAttachedShaders(mesa,3,&attached_count,attached);for(unsigned i=0;i<attached_count&&i<3;i++){char label[48]={0};glGetObjectLabel(GL_SHADER,attached[i],sizeof label,NULL,label);snprintf(p->shader_label+strlen(p->shader_label),sizeof p->shader_label-strlen(p->shader_label),"%u:%s%s",attached[i],label,i+1<attached_count?",":"");}
 struct lr_gl_compile *shader=calloc(1,sizeof *shader);if(!shader)goto failure;
 for(unsigned stage=0;stage<2;stage++){*shader=(struct lr_gl_compile){.name=*slot*2+stage+1,.stage=stage?GL_FRAGMENT_SHADER:GL_VERTEX_SHADER,.length=strlen(source[stage])};memcpy(shader->source,source[stage],shader->length+1);struct lr_gpu_request r={.operation=LR_GPU_COMPILE_SHADER_EXTENDED,.address=(uintptr_t)shader,.bytes=sizeof *shader};if(!call_gpu(c,&r)||!shader->status){note("shader_fallback program=%u stage=%u reason=%.350s",mesa,stage,shader->log);free(shader);goto failure;}}
 free(shader);p->link=(struct lr_gl_link){.name=*slot+1,.vertex=*slot*2+1,.fragment=*slot*2+2};GLint n=0;glGetProgramiv(mesa,GL_ACTIVE_ATTRIBUTES,&n);if(n>LR_GL_MAX_ATTRIBUTES){note("program_fallback program=%u reason=attribute-count count=%d",mesa,n);goto failure;}
 unsigned reflected=0;for(int i=0;i<n;i++){GLint size;GLenum type;GLsizei length;char name[256];glGetActiveAttrib(mesa,i,sizeof name,&length,&size,&type,name);if(!strncmp(name,"gl_",3))continue;GLint location=glGetAttribLocation(mesa,name);if(size!=1||length>=64||location<0||location>=32||(type==GL_FLOAT_MAT4&&location>LR_GL_MAX_ATTRIBUTES-4)){note("program_fallback program=%u reason=attribute-reflection count=%d index=%d name=%s size=%d location=%d",mesa,n,i,name,size,location);goto failure;}if(reflected>=LR_GL_MAX_ATTRIBUTES)goto failure;strcpy(p->link.attribute[reflected].name,name);p->link.attribute[reflected].location=location;reflected++;}p->link.attributes=reflected;
 // GL locations at or above the packet's 8 attribute slots (ioquake3 binds up to 11)
 // get a free slot; vertex fetch reads GL state at the real location (gl_location).
 {unsigned used=0;GLint real[LR_GL_MAX_ATTRIBUTES];for(unsigned i=0;i<reflected;i++){real[i]=(GLint)p->link.attribute[i].location;if(real[i]<LR_GL_MAX_ATTRIBUTES)used|=1u<<real[i];}
  for(unsigned i=0;i<reflected;i++){if(real[i]<LR_GL_MAX_ATTRIBUTES)continue;unsigned slot=0;while(slot<LR_GL_MAX_ATTRIBUTES&&(used&(1u<<slot)))slot++;if(slot>=LR_GL_MAX_ATTRIBUTES)goto failure;used|=1u<<slot;p->link.attribute[i].location=slot;}
  char names[LR_GL_MAX_ATTRIBUTES][64];for(unsigned i=0;i<reflected;i++)strcpy(names[i],p->link.attribute[i].name);
  for(unsigned k=0;k<LR_GL_MAX_ATTRIBUTES;k++)p->gl_location[k]=(uint8_t)k;
  for(unsigned i=0;i<reflected;i++)p->gl_location[p->link.attribute[i].location]=(uint8_t)real[i];
  (void)names;}
 struct lr_gpu_request r={.operation=LR_GPU_LINK_PROGRAM,.address=(uintptr_t)&p->link,.bytes=sizeof p->link};if(!call_gpu(c,&r)||!p->link.status){char label[128]={0};glGetObjectLabel(GL_PROGRAM,mesa,sizeof label,NULL,label);note("pipeline_fallback program=%u label=%s reason=%.350s",mesa,label,p->link.log);const char *dir=config("LR_MESA_METAL_CAPTURE_DIR");static unsigned captures;if(dir&&captures++<8){for(unsigned k=0;k<2;k++){char path[1024];if(snprintf(path,sizeof path,"%s/%ld-%u-%u.glsl",dir,(long)getpid(),mesa,k)>=(int)sizeof path)continue;FILE *f=fopen(path,"wb");if(f){fwrite(source[k],1,strlen(source[k]),f);fclose(f);}}}goto failure;}
 p->valid=1;p->blocks=calloc(1,sizeof *p->blocks);if(!p->blocks)goto failure;p->blocks->program=*slot+1;p->extents.program=p->storages.program=*slot+1;
 r=(struct lr_gpu_request){.operation=LR_GPU_QUERY_BLOCKS_EXTENDED,.address=(uintptr_t)p->blocks,.bytes=sizeof *p->blocks};if(!call_gpu(c,&r))goto failure;
 r=(struct lr_gpu_request){.operation=LR_GPU_QUERY_UNIFORM_EXTENTS,.address=(uintptr_t)&p->extents,.bytes=sizeof p->extents};if(!call_gpu(c,&r))goto failure;
 r=(struct lr_gpu_request){.operation=LR_GPU_QUERY_STORAGES,.address=(uintptr_t)&p->storages,.bytes=sizeof p->storages};if(!call_gpu(c,&r))goto failure;
 p->valid=1;char label[128]={0};glGetObjectLabel(GL_PROGRAM,mesa,sizeof label,NULL,label);note("Metal_pipeline program=%u attributes=%u uniforms=%u UBO=%u SSBO=%u label=%s",mesa,p->link.attributes,p->link.uniforms,p->blocks->count,p->storages.count,label);free(source[0]);free(source[1]);atomic_fetch_add(&stats_program_compiles,1);return p;
 failure:atomic_fetch_add(&stats_program_failures,1);if(source[0]&&source[1]&&c->program[*slot].generation==generation){struct gpu_program *failed=&c->program[*slot];if(failed->valid){struct lr_gpu_request r={.operation=LR_GPU_DELETE_PROGRAM,.count=*slot+1};call_gpu(c,&r);}failed->valid=0;}free(source[0]);free(source[1]);return NULL;
}
struct pixel_state {GLint buffer,alignment,row_length,skip_rows,skip_pixels;};
static void pixel_begin(struct pixel_state *s,int pack){GLenum target=pack?GL_PIXEL_PACK_BUFFER:GL_PIXEL_UNPACK_BUFFER;glGetIntegerv(pack?GL_PIXEL_PACK_BUFFER_BINDING:GL_PIXEL_UNPACK_BUFFER_BINDING,&s->buffer);glGetIntegerv(pack?GL_PACK_ALIGNMENT:GL_UNPACK_ALIGNMENT,&s->alignment);glGetIntegerv(pack?GL_PACK_ROW_LENGTH:GL_UNPACK_ROW_LENGTH,&s->row_length);glGetIntegerv(pack?GL_PACK_SKIP_ROWS:GL_UNPACK_SKIP_ROWS,&s->skip_rows);glGetIntegerv(pack?GL_PACK_SKIP_PIXELS:GL_UNPACK_SKIP_PIXELS,&s->skip_pixels);glBindBuffer(target,0);glPixelStorei(pack?GL_PACK_ALIGNMENT:GL_UNPACK_ALIGNMENT,1);glPixelStorei(pack?GL_PACK_ROW_LENGTH:GL_UNPACK_ROW_LENGTH,0);glPixelStorei(pack?GL_PACK_SKIP_ROWS:GL_UNPACK_SKIP_ROWS,0);glPixelStorei(pack?GL_PACK_SKIP_PIXELS:GL_UNPACK_SKIP_PIXELS,0);}
static void pixel_end(const struct pixel_state *s,int pack){glBindBuffer(pack?GL_PIXEL_PACK_BUFFER:GL_PIXEL_UNPACK_BUFFER,s->buffer);glPixelStorei(pack?GL_PACK_ALIGNMENT:GL_UNPACK_ALIGNMENT,s->alignment);glPixelStorei(pack?GL_PACK_ROW_LENGTH:GL_UNPACK_ROW_LENGTH,s->row_length);glPixelStorei(pack?GL_PACK_SKIP_ROWS:GL_UNPACK_SKIP_ROWS,s->skip_rows);glPixelStorei(pack?GL_PACK_SKIP_PIXELS:GL_UNPACK_SKIP_PIXELS,s->skip_pixels);}
static int read_buffer_bytes(GLuint buffer,size_t offset,size_t bytes,void *data){GLint prior,mapped=0,access=0;GLint64 size=0;glGetIntegerv(GL_COPY_READ_BUFFER_BINDING,&prior);glBindBuffer(GL_COPY_READ_BUFFER,buffer);glGetBufferParameteriv(GL_COPY_READ_BUFFER,GL_BUFFER_MAPPED,&mapped);glGetBufferParameteriv(GL_COPY_READ_BUFFER,GL_BUFFER_ACCESS_FLAGS,&access);glGetBufferParameteri64v(GL_COPY_READ_BUFFER,GL_BUFFER_SIZE,&size);int valid=(!mapped||(access&GL_MAP_PERSISTENT_BIT))&&size>=0&&offset<=(uint64_t)size&&bytes<=(uint64_t)size-offset;if(valid&&es_current()){const void *m=bytes?glMapBufferRange(GL_COPY_READ_BUFFER,offset,bytes,GL_MAP_READ_BIT):NULL;if(m){memcpy(data,m,bytes);glUnmapBuffer(GL_COPY_READ_BUFFER);}else if(bytes)valid=0;}else if(valid)glGetBufferSubData(GL_COPY_READ_BUFFER,offset,bytes,data);else if(config("LR_MESA_METAL_TRACE"))note("buffer_read_reject object=%u storage_bytes=%lld offset=%zu requested=%zu mapped=%d access=%x",buffer,(long long)size,offset,bytes,mapped,access);glBindBuffer(GL_COPY_READ_BUFFER,prior);return valid;}
static void read_color(struct bridge_context *c,GLenum draw_buffer,void *pixels){GLint read,buffer;glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING,&read);glGetIntegerv(GL_READ_BUFFER,&buffer);struct pixel_state s;pixel_begin(&s,1);glBindFramebuffer(GL_READ_FRAMEBUFFER,c->draw_framebuffer);GLint source_read;glGetIntegerv(GL_READ_BUFFER,&source_read);glReadBuffer(draw_buffer);struct color_target t=c->targets[0];for(unsigned i=0;i<c->target_count;i++)if(c->targets[i].buffer==draw_buffer)t=c->targets[i];glReadPixels(0,0,c->width,c->height,(t.type==GL_INT||t.type==GL_UNSIGNED_INT||t.type==GL_SHORT||t.type==GL_UNSIGNED_SHORT)?GL_RGBA_INTEGER:GL_RGBA,t.type,pixels);glReadBuffer(source_read);glBindFramebuffer(GL_READ_FRAMEBUFFER,read);glReadBuffer(buffer);pixel_end(&s,1);}
static void read_stencil(struct bridge_context *c,unsigned char *pixels){GLint read;glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING,&read);struct pixel_state state;pixel_begin(&state,1);glBindFramebuffer(GL_READ_FRAMEBUFFER,c->draw_framebuffer);glReadPixels(0,0,c->width,c->height,GL_STENCIL_INDEX,GL_UNSIGNED_BYTE,pixels);glBindFramebuffer(GL_READ_FRAMEBUFFER,read);pixel_end(&state,1);}
static void read_depth(struct bridge_context *c,float *pixels){GLint read;glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING,&read);struct pixel_state state;pixel_begin(&state,1);glBindFramebuffer(GL_READ_FRAMEBUFFER,c->draw_framebuffer);glReadPixels(0,0,c->width,c->height,GL_DEPTH_COMPONENT,GL_FLOAT,pixels);glBindFramebuffer(GL_READ_FRAMEBUFFER,read);pixel_end(&state,1);}
static void present_color(struct bridge_context *c,struct color_target t){
 GLint read,buffer,texture;glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING,&read);glGetIntegerv(GL_READ_BUFFER,&buffer);glGetIntegerv(GL_TEXTURE_BINDING_2D,&texture);GLboolean scissor=glIsEnabled(GL_SCISSOR_TEST),srgb=state_enabled(GL_FRAMEBUFFER_SRGB)&&!es_current();GLenum buffers[4];for(unsigned i=0;i<c->target_count;i++)buffers[i]=c->targets[i].buffer;struct pixel_state s;pixel_begin(&s,0);
 if(!c->transfer_texture)glGenTextures(1,&c->transfer_texture);if(!c->transfer_fbo)glGenFramebuffers(1,&c->transfer_fbo);
 glBindTexture(GL_TEXTURE_2D,c->transfer_texture);glTexImage2D(GL_TEXTURE_2D,0,t.format,c->width,c->height,0,(t.type==GL_INT||t.type==GL_UNSIGNED_INT||t.type==GL_SHORT||t.type==GL_UNSIGNED_SHORT)?GL_RGBA_INTEGER:GL_RGBA,t.type,c->after+t.offset);glBindFramebuffer(GL_READ_FRAMEBUFFER,c->transfer_fbo);glFramebufferTexture2D(GL_READ_FRAMEBUFFER,GL_COLOR_ATTACHMENT0,GL_TEXTURE_2D,c->transfer_texture,0);glReadBuffer(GL_COLOR_ATTACHMENT0);glDisable(GL_SCISSOR_TEST);if(srgb)glDisable(GL_FRAMEBUFFER_SRGB);state_draw_buffer(t.buffer);glBlitFramebuffer(0,0,c->width,c->height,0,0,c->width,c->height,GL_COLOR_BUFFER_BIT,GL_NEAREST);if(scissor)glEnable(GL_SCISSOR_TEST);if(srgb)glEnable(GL_FRAMEBUFFER_SRGB);glDrawBuffers(c->target_count,buffers);
 glBindFramebuffer(GL_READ_FRAMEBUFFER,read);glReadBuffer(buffer);glBindTexture(GL_TEXTURE_2D,texture);pixel_end(&s,0);
}
// Indexed SIZE is zero for glBindBufferBase: resolve the entire data store.
static GLint64 indexed_bytes(GLuint buffer,GLint64 offset,GLint64 size){if(size||offset||!buffer)return size;GLint previous;GLint64 result=0;glGetIntegerv(GL_COPY_READ_BUFFER_BINDING,&previous);glBindBuffer(GL_COPY_READ_BUFFER,buffer);glGetBufferParameteri64v(GL_COPY_READ_BUFFER,GL_BUFFER_SIZE,&result);glBindBuffer(GL_COPY_READ_BUFFER,previous);return result;}
// Fixed-function draws (guest_fixed_function.inc) supply their uniform block from memory.
static LR_TLS GLuint ff_block_program;static LR_TLS const void *ff_block_data;static LR_TLS size_t ff_block_bytes;
static int upload_blocks(struct bridge_context *c,struct gpu_program *p,unsigned slot){
 if(p->blocks->count){size_t bytes=sizeof(struct lr_gl_block_data);for(unsigned i=0;i<p->blocks->count;i++)bytes+=p->blocks->block[i].bytes;struct lr_gl_block_data *packet=calloc(1,bytes);if(!packet)return 0;packet->program=slot+1;packet->count=p->blocks->count;size_t at=sizeof *packet;int ok=1;
  for(unsigned i=0;i<p->blocks->count;i++){const struct lr_gl_block *b=&p->blocks->block[i];if(ff_block_program&&p->mesa==ff_block_program&&ff_block_data&&ff_block_bytes>=b->bytes){memcpy((char *)packet+at,ff_block_data,b->bytes);packet->block[i].offset=at;packet->block[i].bytes=b->bytes;at+=b->bytes;continue;}
  GLuint index=glGetUniformBlockIndex(p->mesa,b->name);GLint binding=0,buffer=0;GLint64 offset=0,size=0;if(index==GL_INVALID_INDEX){ok=0;break;}glGetActiveUniformBlockiv(p->mesa,index,GL_UNIFORM_BLOCK_BINDING,&binding);glGetIntegeri_v(GL_UNIFORM_BUFFER_BINDING,binding,&buffer);glGetInteger64i_v(GL_UNIFORM_BUFFER_START,binding,&offset);glGetInteger64i_v(GL_UNIFORM_BUFFER_SIZE,binding,&size);size=indexed_bytes(buffer,offset,size);if(!buffer||offset<0||size<b->bytes||!read_buffer_bytes(buffer,offset,b->bytes,(char *)packet+at)){note("UBO_fallback program=%u block=%s binding=%d buffer=%d offset=%lld size=%lld required=%u",p->mesa,b->name,binding,buffer,(long long)offset,(long long)size,b->bytes);ok=0;break;}packet->block[i].offset=at;packet->block[i].bytes=b->bytes;at+=b->bytes;}
  if(ok){struct lr_gpu_request r={.operation=LR_GPU_UNIFORM_BLOCK_DATA,.address=(uintptr_t)packet,.bytes=bytes};ok=call_gpu(c,&r);}free(packet);if(!ok)return 0;
 }
 if(p->storages.count){size_t bytes=sizeof(struct lr_gl_storage_data);size_t sizes[LR_GL_MAX_STORAGE_BLOCKS];GLint buffers[LR_GL_MAX_STORAGE_BLOCKS];GLint64 offsets[LR_GL_MAX_STORAGE_BLOCKS];
  for(unsigned i=0;i<p->storages.count;i++){const struct lr_gl_storage *b=&p->storages.block[i];GLuint index=glGetProgramResourceIndex(p->mesa,GL_SHADER_STORAGE_BLOCK,b->name);GLint binding=0;GLenum property=GL_BUFFER_BINDING;GLint64 size=0;if(index==GL_INVALID_INDEX)return 0;glGetProgramResourceiv(p->mesa,GL_SHADER_STORAGE_BLOCK,index,1,&property,1,NULL,&binding);glGetIntegeri_v(GL_SHADER_STORAGE_BUFFER_BINDING,binding,&buffers[i]);glGetInteger64i_v(GL_SHADER_STORAGE_BUFFER_START,binding,&offsets[i]);glGetInteger64i_v(GL_SHADER_STORAGE_BUFFER_SIZE,binding,&size);size=indexed_bytes(buffers[i],offsets[i],size);if(!buffers[i]||offsets[i]<0||size<b->min_bytes||size>LR_GL_MAX_STORAGE_BYTES)return 0;sizes[i]=size;bytes+=size;}
  struct lr_gl_storage_data *packet=calloc(1,bytes);if(!packet)return 0;packet->program=slot+1;packet->count=p->storages.count;size_t at=sizeof *packet;int ok=1;for(unsigned i=0;i<p->storages.count;i++){if(!read_buffer_bytes(buffers[i],offsets[i],sizes[i],(char *)packet+at)){ok=0;break;}packet->block[i].offset=at;packet->block[i].bytes=sizes[i];at+=sizes[i];}if(ok){struct lr_gpu_request r={.operation=LR_GPU_STORAGE_DATA,.address=(uintptr_t)packet,.bytes=bytes};ok=call_gpu(c,&r);}free(packet);if(!ok)return 0;
 }return 1;
}
static int upload_sampler(struct bridge_context *c,GLint unit,unsigned *gpu_name){
 if(unit<0||unit>=LR_GPU_TEXTURE_UNITS)return 0;GLint prior,texture,internal=0,w=0,h=0,min=0,mag=0,ws=0,wt=0,swizzle[4],sampler=0;glGetIntegerv(GL_ACTIVE_TEXTURE,&prior);glActiveTexture(GL_TEXTURE0+unit);glGetIntegerv(GL_TEXTURE_BINDING_2D,&texture);glGetIntegeri_v(GL_SAMPLER_BINDING,unit,&sampler);int ok=0;struct lr_gpu_texture *packet=NULL;if(!texture)goto done;
 glGetTexLevelParameteriv(GL_TEXTURE_2D,0,GL_TEXTURE_INTERNAL_FORMAT,&internal);glGetTexLevelParameteriv(GL_TEXTURE_2D,0,GL_TEXTURE_WIDTH,&w);glGetTexLevelParameteriv(GL_TEXTURE_2D,0,GL_TEXTURE_HEIGHT,&h);
 if(w<1||h<1||w>LR_GPU_MAX_DIMENSION||h>LR_GPU_MAX_DIMENSION||(internal!=GL_RGBA8&&internal!=GL_RGB8&&internal!=GL_R8&&internal!=GL_RG8&&internal!=GL_RGBA&&internal!=GL_RGB&&internal!=GL_RGBA16F&&internal!=GL_SRGB8_ALPHA8&&internal!=GL_DEPTH_COMPONENT32F&&internal!=GL_DEPTH_COMPONENT24&&internal!=GL_DEPTH_COMPONENT16&&internal!=GL_DEPTH24_STENCIL8&&internal!=GL_DEPTH32F_STENCIL8))goto done;
 GLint base_level=0;glGetTexParameteriv(GL_TEXTURE_2D,GL_TEXTURE_BASE_LEVEL,&base_level);if(base_level)goto done;
 if(sampler){glGetSamplerParameteriv(sampler,GL_TEXTURE_MIN_FILTER,&min);glGetSamplerParameteriv(sampler,GL_TEXTURE_MAG_FILTER,&mag);glGetSamplerParameteriv(sampler,GL_TEXTURE_WRAP_S,&ws);glGetSamplerParameteriv(sampler,GL_TEXTURE_WRAP_T,&wt);}else{glGetTexParameteriv(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,&min);glGetTexParameteriv(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,&mag);glGetTexParameteriv(GL_TEXTURE_2D,GL_TEXTURE_WRAP_S,&ws);glGetTexParameteriv(GL_TEXTURE_2D,GL_TEXTURE_WRAP_T,&wt);}glGetTexParameteriv(GL_TEXTURE_2D,GL_TEXTURE_SWIZZLE_RGBA,swizzle);
 if((min!=GL_NEAREST&&min!=GL_LINEAR)||(mag!=GL_NEAREST&&mag!=GL_LINEAR)||swizzle[0]!=GL_RED||swizzle[1]!=GL_GREEN||swizzle[2]!=GL_BLUE||swizzle[3]!=GL_ALPHA)goto done;
 /* non-power-of-two repeat is valid desktop GL and Metal */
 int hdr=internal==GL_RGBA16F;int srgb=internal==GL_SRGB8_ALPHA8;size_t bytes=sizeof *packet+(size_t)w*h*(hdr?8:4);packet=malloc(bytes);if(!packet)goto done;
 *packet=(struct lr_gpu_texture){.name=unit+1,.width=w,.height=h,.min_filter=min,.mag_filter=mag,.wrap_s=ws,.wrap_t=wt};struct pixel_state pack;pixel_begin(&pack,1);int depth=internal==GL_DEPTH_COMPONENT32F||internal==GL_DEPTH_COMPONENT24||internal==GL_DEPTH_COMPONENT16||internal==GL_DEPTH24_STENCIL8||internal==GL_DEPTH32F_STENCIL8;if(es_current()){   /* ES has no glGetTexImage: read level 0 through a framebuffer (8-bit colour images only) */
  if(depth||hdr){pixel_end(&pack,1);goto done;}GLint read=0,read_buffer=GL_BACK;glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING,&read);glGetIntegerv(GL_READ_BUFFER,&read_buffer);if(!c->transfer_fbo)glGenFramebuffers(1,&c->transfer_fbo);
  glBindFramebuffer(GL_READ_FRAMEBUFFER,c->transfer_fbo);glFramebufferTexture2D(GL_READ_FRAMEBUFFER,GL_COLOR_ATTACHMENT0,GL_TEXTURE_2D,texture,0);glReadBuffer(GL_COLOR_ATTACHMENT0);
  int complete=glCheckFramebufferStatus(GL_READ_FRAMEBUFFER)==GL_FRAMEBUFFER_COMPLETE;if(complete)glReadPixels(0,0,w,h,GL_RGBA,GL_UNSIGNED_BYTE,packet->pixels);
  glFramebufferTexture2D(GL_READ_FRAMEBUFFER,GL_COLOR_ATTACHMENT0,GL_TEXTURE_2D,0,0);glBindFramebuffer(GL_READ_FRAMEBUFFER,read);glReadBuffer(read_buffer);pixel_end(&pack,1);if(!complete)goto done;}
 else{glGetTexImage(GL_TEXTURE_2D,0,depth?GL_DEPTH_COMPONENT:GL_RGBA,hdr?GL_HALF_FLOAT:(depth?GL_FLOAT:GL_UNSIGNED_BYTE),packet->pixels);pixel_end(&pack,1);}
 struct lr_gpu_request r={.operation=LR_GPU_TEXTURE,.width=w,.height=h,.count=depth?GL_DEPTH_COMPONENT:(hdr?GL_RGBA16F:(srgb?GL_SRGB8_ALPHA8:GL_RGBA)),.address=(uintptr_t)packet,.bytes=bytes};ok=call_gpu(c,&r);if(ok)*gpu_name=unit+1;
 done:if(!ok)note("texture_fallback internal=%x dimensions=%dx%d min=%x mag=%x wrap=%x,%x",internal,w,h,min,mag,ws,wt);free(packet);glActiveTexture(prior);return ok;
}
static unsigned resident_sampler(struct bridge_context *c,GLint unit,const struct resident_targets *targets);
static LR_TLS const struct resident_targets *uniform_targets;
static int uniforms(struct bridge_context *c,struct gpu_program *p,unsigned slot,struct lr_gl_draw *draw){
 for(unsigned i=0;i<p->link.uniforms;i++){const struct lr_gl_variable *u=&p->link.uniform[i];if(!p->located[i]){p->location[i]=glGetUniformLocation(p->mesa,u->name);p->located[i]=1;}GLint location=p->location[i];uint64_t sampler_started=0;if(location<0){note("uniform_fallback program=%u name=%s reason=absent_Mesa_location",p->mesa,u->name);report_reasonf("u:absent-location");return 0;}unsigned kind=lr_gl_type_kind(u->type),components=lr_gl_type_components(u->type),elements=p->extents.uniform[i].elements;
  if(kind==4){sampler_started=atomic_load_explicit(&report_phases,memory_order_relaxed)?report_now():0;GLint unit;glGetUniformiv(p->mesa,location,&unit);unsigned texture=uniform_targets&&elements==1?resident_sampler(c,unit,uniform_targets):0;if(texture){draw->uniform[i][0]=texture;if(sampler_started)report_add(REPORT_P_U_SAMPLERS,sampler_started,0);continue;}if(uniform_targets&&unit>=0&&unit<LR_GPU_TEXTURE_UNITS){GLint prior,name=0;glGetIntegerv(GL_ACTIVE_TEXTURE,&prior);glActiveTexture(GL_TEXTURE0+unit);glGetIntegerv(GL_TEXTURE_BINDING_2D,&name);glActiveTexture(prior);if(name)resident_sync(c,(GLuint)name,1);if(!resident_is_bound(c)){note("sampler_fallback program=%u name=%s unit=%d reason=resident-feedback",p->mesa,u->name,unit);report_reasonf("u:sampler-feedback");return 0;}}if(elements!=1||!upload_sampler(c,unit,&texture)){note("sampler_fallback program=%u name=%s unit=%d",p->mesa,u->name,unit);report_reasonf(elements!=1?"u:sampler-array":"u:sampler-unit%d",unit);return 0;}draw->uniform[i][0]=texture;continue;}
  if(elements==1){if(kind==2)glGetUniformuiv(p->mesa,location,(GLuint *)draw->uniform[i]);else if(kind)glGetUniformiv(p->mesa,location,(GLint *)draw->uniform[i]);else glGetUniformfv(p->mesa,location,draw->uniform[i]);}
  else{size_t bytes=sizeof(struct lr_gl_uniform_array_data)+(size_t)elements*components*4;struct lr_gl_uniform_array_data *packet=calloc(1,bytes);if(!packet)return 0;*packet=(struct lr_gl_uniform_array_data){.program=slot+1,.index=i,.count=elements};int ok=1;char base[64];snprintf(base,sizeof base,"%s",u->name);char *array=strstr(base,"[0]");if(array)*array=0;
   for(unsigned j=0;j<elements;j++){char name[96];snprintf(name,sizeof name,"%s[%u]",base,j);GLint at=glGetUniformLocation(p->mesa,name);if(at<0)continue;uint32_t *data=packet->data+j*components;if(kind==2)glGetUniformuiv(p->mesa,at,data);else if(kind)glGetUniformiv(p->mesa,at,(GLint *)data);else glGetUniformfv(p->mesa,at,(float *)data);}
   if(ok){struct lr_gpu_request r={.operation=LR_GPU_UNIFORM_ARRAY_DATA,.address=(uintptr_t)packet,.bytes=bytes};ok=call_gpu(c,&r);}free(packet);if(!ok){report_reasonf("u:array-data-errno%d-n%u-c%u-k%u",errno,elements,components,kind);return 0;}
  }
 }uint64_t blocks_started=atomic_load_explicit(&report_phases,memory_order_relaxed)?report_now():0;if(!upload_blocks(c,p,slot)){report_reasonf("u:blocks");return 0;}if(blocks_started)report_add(REPORT_P_U_BLOCKS,blocks_started,0);return 1;
}
static unsigned component_bytes(GLenum type){switch(type){case GL_INT_2_10_10_10_REV:case GL_UNSIGNED_INT_2_10_10_10_REV:case GL_FLOAT:case GL_INT:case GL_UNSIGNED_INT:return 4;case GL_SHORT:case GL_UNSIGNED_SHORT:return 2;case GL_BYTE:case GL_UNSIGNED_BYTE:return 1;default:return 0;}}
static void attribute_value(const unsigned char *data,GLenum type,int normalized,unsigned kind,float value[4],unsigned n){
 if(type==GL_FLOAT&&!kind){memcpy(value,data,(size_t)n*4);return;}   // common case: plain floats
 if(type==GL_UNSIGNED_BYTE&&normalized&&!kind){for(unsigned i=0;i<n;i++)value[i]=(float)(data[i]/255.0);return;}   // byte colours
 if(type==GL_INT_2_10_10_10_REV||type==GL_UNSIGNED_INT_2_10_10_10_REV){uint32_t packed;memcpy(&packed,data,4);for(unsigned i=0;i<n;i++){unsigned bits=i==3?2:10,mask=(1u<<bits)-1,raw=(packed>>(i*10))&mask;if(type==GL_INT_2_10_10_10_REV){int32_t v=(int32_t)(raw<<(32-bits))>>(32-bits);value[i]=normalized?fmaxf(-1,(float)v/((1u<<(bits-1))-1)):v;}else value[i]=normalized?(float)raw/mask:raw;}return;}
 for(unsigned i=0;i<n;i++){double x=0;int32_t signed_value=0;uint32_t unsigned_value=0;switch(type){case GL_FLOAT:{float f;memcpy(&f,data+i*4,4);x=f;break;}case GL_INT:memcpy(&signed_value,data+i*4,4);x=normalized?fmax(-1,(double)signed_value/2147483647):signed_value;break;case GL_UNSIGNED_INT:memcpy(&unsigned_value,data+i*4,4);x=normalized?(double)unsigned_value/4294967295:unsigned_value;break;case GL_SHORT:{int16_t v;memcpy(&v,data+i*2,2);signed_value=v;x=normalized?fmax(-1,(double)v/32767):v;break;}case GL_UNSIGNED_SHORT:{uint16_t v;memcpy(&v,data+i*2,2);unsigned_value=v;x=normalized?(double)v/65535:v;break;}case GL_BYTE:signed_value=(int8_t)data[i];x=normalized?fmax(-1,(double)signed_value/127):signed_value;break;case GL_UNSIGNED_BYTE:unsigned_value=data[i];x=normalized?(double)data[i]/255:data[i];break;}
  int is_unsigned=type==GL_UNSIGNED_BYTE||type==GL_UNSIGNED_SHORT||type==GL_UNSIGNED_INT;if(kind==1){int32_t v=is_unsigned?(int32_t)unsigned_value:signed_value;memcpy(value+i,&v,4);}else if(kind==2){uint32_t v=is_unsigned?unsigned_value:(uint32_t)signed_value;memcpy(value+i,&v,4);}else value[i]=x;
 }
}
// First vertex of the packet: draws are rebased so only the vertices they use
// travel (index values beyond the packet limit become possible).
static LR_TLS unsigned draw_vertex_base;
static int vertices(struct gpu_program *p,struct lr_gl_draw *draw,unsigned instances,unsigned base,struct lr_gpu_instance_attributes *inst){
 *inst=(struct lr_gpu_instance_attributes){.instances=instances+base};
 for(unsigned i=0;i<p->link.attributes;i++){const struct lr_gl_variable *a=&p->link.attribute[i];unsigned columns=a->type==GL_FLOAT_MAT4?4:1;for(unsigned column=0;column<columns;column++){unsigned location=a->location+column,kind=lr_gl_type_kind(a->type),components=columns==4?4:lr_gl_type_components(a->type);if(components>4||location>=LR_GL_MAX_ATTRIBUTES)return 0;
  unsigned gl=p->gl_location[a->location]+column;   // GL attribute location (packet slot may differ)
  GLint enabled=0,size=0,type=0,stride=0,buffer=0,normalized=0,integer=0,divisor=0,binding=0,binding_stride=0,relative_offset=0;GLint64 binding_offset=0;glGetVertexAttribiv(gl,GL_VERTEX_ATTRIB_ARRAY_ENABLED,&enabled);glGetVertexAttribiv(gl,GL_VERTEX_ATTRIB_ARRAY_DIVISOR,&divisor);glGetVertexAttribiv(gl,GL_VERTEX_ATTRIB_BINDING,&binding);glGetVertexAttribiv(gl,GL_VERTEX_ATTRIB_RELATIVE_OFFSET,&relative_offset);glGetIntegeri_v(GL_VERTEX_BINDING_STRIDE,binding,&binding_stride);glGetInteger64i_v(GL_VERTEX_BINDING_OFFSET,binding,&binding_offset);
  if(!enabled){float value[4];if(kind==1)glGetVertexAttribIiv(gl,GL_CURRENT_VERTEX_ATTRIB,(GLint *)value);else if(kind==2)glGetVertexAttribIuiv(gl,GL_CURRENT_VERTEX_ATTRIB,(GLuint *)value);else glGetVertexAttribfv(gl,GL_CURRENT_VERTEX_ATTRIB,value);for(unsigned j=0;j<draw->count;j++)memcpy(draw->vertex[j].attribute[location],value,16);continue;}
  glGetVertexAttribiv(gl,GL_VERTEX_ATTRIB_ARRAY_SIZE,&size);glGetVertexAttribiv(gl,GL_VERTEX_ATTRIB_ARRAY_TYPE,&type);glGetVertexAttribiv(gl,GL_VERTEX_ATTRIB_ARRAY_STRIDE,&stride);glGetVertexAttribiv(gl,GL_VERTEX_ATTRIB_ARRAY_BUFFER_BINDING,&buffer);glGetVertexAttribiv(gl,GL_VERTEX_ATTRIB_ARRAY_NORMALIZED,&normalized);glGetVertexAttribiv(gl,GL_VERTEX_ATTRIB_ARRAY_INTEGER,&integer);
  int bgra=size==GL_BGRA;if(bgra){if(type!=GL_UNSIGNED_BYTE||!normalized)return 0;size=4;}   /* GL_BGRA: red and blue swapped */
  unsigned cb=component_bytes(type);
  // No buffer object: a client-memory array of this process (compatibility contexts).
  if(!buffer&&cb&&size>=1&&size<=4&&!divisor&&!(kind&&!integer)&&!(!kind&&integer)&&type!=GL_INT_2_10_10_10_REV&&type!=GL_UNSIGNED_INT_2_10_10_10_REV){
   void *pointer=NULL;glGetVertexAttribPointerv(gl,GL_VERTEX_ATTRIB_ARRAY_POINTER,&pointer);if(!pointer)return 0;
   size_t element=(size_t)cb*size,step=stride?(size_t)stride:element;const unsigned char *base=(const unsigned char *)pointer+(size_t)draw_vertex_base*step;
   for(unsigned j=0;j<draw->count;j++){float *value=draw->vertex[j].attribute[location];if(kind){uint32_t one=1;memset(value,0,16);memcpy(value+3,&one,4);}else{value[0]=value[1]=value[2]=0;value[3]=1;}attribute_value(base+(size_t)j*step,type,normalized,kind,value,size);if(bgra){float t=value[0];value[0]=value[2];value[2]=t;}}
   continue;}
  if(!buffer||!cb||size<1||size>4||(kind&&!integer)||(!kind&&integer)||divisor<0){if(config("LR_MESA_METAL_TRACE"))note("vertex_fallback reason=format program=%u name=%s location=%u shader_type=%x array_type=%x size=%d integer=%d divisor=%d buffer=%d",p->mesa,a->name,location,a->type,type,size,integer,divisor,buffer);return 0;}unsigned packed=(type==GL_INT_2_10_10_10_REV||type==GL_UNSIGNED_INT_2_10_10_10_REV);if(packed&&(size!=4||integer))return 0;unsigned element_bytes=packed?4:size*cb;
  // Fetch through the attribute's vertex buffer binding (GL 4.3
  // ARB_vertex_attrib_binding), which legacy glVertexAttribPointer is defined
  // in terms of: source = binding offset + relative offset, step = binding
  // stride (Mesa stores the packed stride for a legacy stride of 0). A binding
  // stride of 0 repeats one element for every vertex and instance (Blender
  // binds placeholder attributes this way); the legacy stride and pointer
  // queries cannot express either case.
  if(binding_offset<0||relative_offset<0||(uint64_t)binding_offset>SIZE_MAX-(uint64_t)relative_offset-element_bytes)return 0;
  size_t source_offset=(size_t)binding_offset+(size_t)relative_offset;int constant=binding_stride==0;stride=binding_stride;
  if(!constant&&!divisor)source_offset+=(size_t)draw_vertex_base*(size_t)stride;
  if(config("LR_MESA_METAL_TRACE"))note("vertex_binding program=%u name=%s location=%u buffer=%d binding=%d offset=%lld relative=%d stride=%d constant=%d",p->mesa,a->name,location,buffer,binding,(long long)binding_offset,relative_offset,binding_stride,constant);
  unsigned elements=constant?1:(divisor?base+(instances-1)/(unsigned)divisor+1:draw->count);size_t bytes=(size_t)(elements-1)*stride+element_bytes;if(bytes>4u*1024u*1024u)return 0;unsigned char *data=malloc(bytes);if(!data)return 0;int ok=read_buffer_bytes(buffer,source_offset,bytes,data);if(!ok){if(config("LR_MESA_METAL_TRACE"))note("vertex_fallback reason=buffer-range program=%u name=%s location=%u buffer=%d offset=%llu bytes=%zu vertices=%u",p->mesa,a->name,location,buffer,(unsigned long long)source_offset,bytes,draw->count);free(data);return 0;}
  if(divisor){int vertex_storage=0;for(unsigned b=0;b<p->storages.count;b++)if(p->storages.block[b].vertex_binding!=UINT32_MAX)vertex_storage=1;
   if(vertex_storage){
    // SSBO slots overlap the legacy per-instance vertex buffer slots. For a
    // single instance the fetched attribute is constant across every vertex:
    // snapshot it into the canonical vertex stream, without rebinding storage.
    // Multi-instance draws retain Mesa until these binding ranges are separated.
    if(instances!=1){note("vertex_fallback reason=instance-storage-binding-conflict location=%u instances=%u",location,instances);free(data);return 0;}
    float value[4]={0,0,0,1};if(kind){uint32_t one=1;memcpy(value+3,&one,4);}attribute_value(data+(size_t)base*stride,type,normalized,kind,value,size);for(unsigned j=0;j<draw->count;j++)memcpy(draw->vertex[j].attribute[location],value,16);free(data);continue;
   }
   inst->divisor[location]=1;inst->count[location]=base+instances;for(unsigned j=base;j<base+instances;j++){float *value=inst->value[location][j];if(kind){uint32_t one=1;memcpy(value+3,&one,4);}else value[3]=1;attribute_value(data+((j-base)/divisor+base)*stride,type,normalized,kind,value,size);}}
  else for(unsigned j=0;j<draw->count;j++){float *value=draw->vertex[j].attribute[location];if(kind){uint32_t one=1;memcpy(value+3,&one,4);}else value[3]=1;attribute_value(data+(size_t)(constant?0:j)*stride,type,normalized,kind,value,size);if(bgra){float t=value[0];value[0]=value[2];value[2]=t;}}
  free(data);
 }}return 1;
}
struct draw_call {GLenum mode,type;GLint first,base_vertex;GLsizei count,instances;GLuint base,range_start,range_end;const void *indices,*indirect;unsigned kind;};
static int read_buffer_bytes(GLuint buffer,size_t offset,size_t bytes,void *data);
static void resident_around_mesa_draw(int after,GLenum mode);
static void mesa_draw(const struct draw_call *d){resident_around_mesa_draw(0,d->mode);switch(d->kind){case 0:{typeof(&glDrawArrays) f=original("glDrawArrays");if(f)f(d->mode,d->first,d->count);break;}case 1:{typeof(&glDrawArraysInstanced) f=original("glDrawArraysInstanced");if(f)f(d->mode,d->first,d->count,d->instances);break;}case 2:{typeof(&glDrawArraysInstancedBaseInstance) f=original("glDrawArraysInstancedBaseInstance");if(f)f(d->mode,d->first,d->count,d->instances,d->base);break;}case 3:{typeof(&glDrawElements) f=original("glDrawElements");if(f)f(d->mode,d->count,d->type,d->indices);break;}case 4:{typeof(&glDrawElementsInstanced) f=original("glDrawElementsInstanced");if(f)f(d->mode,d->count,d->type,d->indices,d->instances);break;}case 5:{typeof(&glDrawElementsInstancedBaseVertexBaseInstance) f=original("glDrawElementsInstancedBaseVertexBaseInstance");if(f)f(d->mode,d->count,d->type,d->indices,d->instances,d->base_vertex,d->base);break;}case 6:{typeof(&glDrawElementsBaseVertex) f=original("glDrawElementsBaseVertex");if(f)f(d->mode,d->count,d->type,d->indices,d->base_vertex);break;}case 7:{typeof(&glDrawElementsInstancedBaseVertex) f=original("glDrawElementsInstancedBaseVertex");if(f)f(d->mode,d->count,d->type,d->indices,d->instances,d->base_vertex);break;}case 8:{typeof(&glDrawArraysIndirect) f=original("glDrawArraysIndirect");if(f)f(d->mode,d->indirect);break;}case 9:{typeof(&glDrawElementsIndirect) f=original("glDrawElementsIndirect");if(f)f(d->mode,d->type,d->indirect);break;}case 10:{typeof(&glDrawRangeElements) f=original("glDrawRangeElements");if(f)f(d->mode,d->range_start,d->range_end,d->count,d->type,d->indices);break;}}resident_around_mesa_draw(1,0);}
static unsigned draw_limit(void){const char *s=config("LR_MESA_METAL_MAX_DRAWS");if(!s)return 16;if(!strcmp(s,"unlimited"))return UINT_MAX;   // explicit opt-in; large numbers stay clamped
 char *end;unsigned long n=strtoul(s,&end,10);return *end||n>10000?16:n;}
// Process-wide cost/fallback accounting, read and reset by lrMesaMetalReport
// (benchmarks call it through dlsym; it never affects rendering).
static pthread_mutex_t report_lock=PTHREAD_MUTEX_INITIALIZER;
static struct {const char *reason;uint64_t count;} report_reasons[64];
static uint64_t report_ns[REPORT_SLOTS],report_count[REPORT_SLOTS],report_bytes[REPORT_SLOTS];
// Inactive (no clock reads, no locking) until a benchmark first calls lrMesaMetalReport.
static _Atomic int report_enabled;   // phases: per-draw timers, benchmarks only (lrMesaMetalReport)
static void stats_at_exit(void){stats_write(1);}
static int report_wanted(void){   // counters also run for LR_MESA_METAL_STATS_DIR sessions, from the first GL use
 if(atomic_load_explicit(&report_enabled,memory_order_relaxed))return 1;
 static int stats=-1;if(stats<0){stats=config("LR_MESA_METAL_STATS_DIR")&&*config("LR_MESA_METAL_STATS_DIR");if(stats)atexit(stats_at_exit);}   // final numbers even without a swap
 if(stats)atomic_store(&report_enabled,1);return stats;
}
static uint64_t report_now(void){if(!report_wanted())return 0;struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return (uint64_t)t.tv_sec*1000000000ull+(uint64_t)t.tv_nsec;}
static void report_add(unsigned slot,uint64_t started,uint64_t bytes){if(!started)return;uint64_t ns=report_now()-started;pthread_mutex_lock(&report_lock);report_ns[slot]+=ns;report_count[slot]++;report_bytes[slot]+=bytes;pthread_mutex_unlock(&report_lock);}
static void report_reason(const char *reason){if(!report_wanted())return;pthread_mutex_lock(&report_lock);for(unsigned i=0;i<64;i++){if(!report_reasons[i].reason)report_reasons[i].reason=reason;if(report_reasons[i].reason==reason||!strcmp(report_reasons[i].reason,reason)){report_reasons[i].count++;break;}}pthread_mutex_unlock(&report_lock);}
// Formatted reasons are interned (bounded) so the table can keep pointers.
static void report_reasonf(const char *format,...){
 if(!report_wanted())return;
 static char interned[48][48];static unsigned used;char text[48];va_list args;va_start(args,format);vsnprintf(text,sizeof text,format,args);va_end(args);
 pthread_mutex_lock(&report_lock);const char *reason=NULL;for(unsigned i=0;i<used;i++)if(!strcmp(interned[i],text)){reason=interned[i];break;}
 if(!reason&&used<48){strcpy(interned[used],text);reason=interned[used++];}pthread_mutex_unlock(&report_lock);
 report_reason(reason?reason:"other");
}
int lrMesaMetalReport(char *out,size_t size){
 atomic_store(&report_enabled,1);atomic_store(&report_phases,1);
 static const char *names[REPORT_SLOTS]={"upload","writeback","writeback_window","wb_read","gpu_draw","mesa_draw","p_state","p_targets","p_program","p_uniforms","p_vertices","p_state_ioctl","p_draw_ioctl","p_ff","p_lookup","p_indices","p_u_samplers","p_u_blocks"};size_t used=0;if(!out||!size)return 0;out[0]=0;
 pthread_mutex_lock(&report_lock);
 for(unsigned i=0;i<REPORT_SLOTS&&used<size;i++)used+=snprintf(out+used,size-used,"%s=%llu/%.1fms/%lluKB ",names[i],(unsigned long long)report_count[i],report_ns[i]/1e6,(unsigned long long)(report_bytes[i]>>10));
 for(unsigned i=0;i<64&&report_reasons[i].reason&&used<size;i++)used+=snprintf(out+used,size-used,"reject:%s=%llu ",report_reasons[i].reason,(unsigned long long)report_reasons[i].count);
 memset(report_reasons,0,sizeof report_reasons);memset(report_ns,0,sizeof report_ns);memset(report_count,0,sizeof report_count);memset(report_bytes,0,sizeof report_bytes);
 pthread_mutex_unlock(&report_lock);return 1;
}
static int line_mode(GLenum mode){return mode==GL_LINES||mode==GL_LINE_STRIP||mode==GL_LINE_LOOP;}
static int draw_rejected(struct bridge_context *c,const struct draw_call *call,const char *reason){
 report_reason(reason);
 unsigned depth=glIsEnabled(GL_DEPTH_TEST);for(unsigned i=0;i<c->rejections;i++){struct rejection *r=&c->rejection[i];if(r->program==c->last_program&&r->mode==call->mode&&r->kind==call->kind&&r->depth==depth&&r->base_vertex==call->base_vertex&&!strcmp(r->reason,reason))return 0;}
 if(c->rejections<128)c->rejection[c->rejections++]=(struct rejection){c->last_program,reason,call->mode,call->kind,depth,call->base_vertex};else return 0;
 if(config("LR_MESA_METAL_TRACE"))fprintf(stderr,"MESA_METAL_FALLBACK context_id=%llu program=%u label=%s reason=%s mode=%x count=%d instances=%d framebuffer=%d depth=%d stencil=%d\n",(unsigned long long)c->id,c->last_program,c->last_program_label,reason,call->mode,call->count,call->instances,c->draw_framebuffer,depth,glIsEnabled(GL_STENCIL_TEST));
 note("draw_reject reason=%s context_id=%llu program=%u program_label=%s shader_label=%s framebuffer=%d mode=%x count=%d first=%d instances=%d base=%u base_vertex=%d kind=%u depth=%d",reason,(unsigned long long)c->id,c->last_program,c->last_program_label,c->last_shader_label,c->draw_framebuffer,call->mode,call->count,call->first,call->instances,call->base,call->base_vertex,call->kind,depth);
 return 0;
}
static int collect_targets_uncached(struct bridge_context *c,GLint fb,unsigned *width,unsigned *height);
static int collect_targets(struct bridge_context *c,GLint fb,unsigned *width,unsigned *height){
 if(fb)return collect_targets_uncached(c,fb,width,height);
 // Window framebuffer: reuse the last result while the draw buffers, sRGB enable and size match.
 struct window_targets *m=&c->window_targets;GLint buffer=GL_NONE,second=GL_NONE;glGetIntegerv(GL_DRAW_BUFFER0,&buffer);glGetIntegerv(GL_DRAW_BUFFER1,&second);unsigned srgb=state_enabled(GL_FRAMEBUFFER_SRGB),w=0,h=0;window_size(&w,&h);
 if(m->valid&&m->buffer==buffer&&m->second==second&&m->srgb==srgb&&m->width==w&&m->height==h){c->target_count=m->count;c->color_bytes=m->color_bytes;c->framebuffer_srgb=m->framebuffer_srgb;c->targets[0]=m->target;*width=w;*height=h;return 1;}
 m->valid=0;if(!collect_targets_uncached(c,fb,width,height))return 0;
 if(c->target_count==1&&*width==w&&*height==h)*m=(struct window_targets){.valid=1,.buffer=buffer,.second=second,.srgb=srgb,.width=w,.height=h,.count=1,.framebuffer_srgb=c->framebuffer_srgb,.color_bytes=c->color_bytes,.target=c->targets[0]};
 return 1;
}
static int collect_targets_uncached(struct bridge_context *c,GLint fb,unsigned *width,unsigned *height){
 c->target_count=0;c->color_bytes=0;for(unsigned i=0;i<4;i++){
  GLint buffer;glGetIntegerv(GL_DRAW_BUFFER0+i,&buffer);if(buffer==GL_NONE)break;
  if(!fb&&buffer==GL_BACK_LEFT){GLboolean stereo=GL_FALSE;if(!es_current())glGetBooleanv(GL_STEREO,&stereo);if(!stereo)buffer=GL_BACK;}   // mono visual: the same buffer
if(fb?(buffer<GL_COLOR_ATTACHMENT0||buffer>GL_COLOR_ATTACHMENT15):(i|| (buffer!=GL_FRONT&&buffer!=GL_BACK))){report_reasonf("t:buffer-%x",buffer);return 0;}
  GLenum attachment=fb?buffer:buffer==GL_BACK?GL_BACK_LEFT:GL_FRONT_LEFT;GLint encoding,component,bits[4];GLenum properties[]={GL_FRAMEBUFFER_ATTACHMENT_RED_SIZE,GL_FRAMEBUFFER_ATTACHMENT_GREEN_SIZE,GL_FRAMEBUFFER_ATTACHMENT_BLUE_SIZE,GL_FRAMEBUFFER_ATTACHMENT_ALPHA_SIZE};
  glGetFramebufferAttachmentParameteriv(GL_DRAW_FRAMEBUFFER,attachment,GL_FRAMEBUFFER_ATTACHMENT_COLOR_ENCODING,&encoding);glGetFramebufferAttachmentParameteriv(GL_DRAW_FRAMEBUFFER,attachment,GL_FRAMEBUFFER_ATTACHMENT_COMPONENT_TYPE,&component);for(unsigned j=0;j<4;j++)glGetFramebufferAttachmentParameteriv(GL_DRAW_FRAMEBUFFER,attachment,properties[j],bits+j);
  struct color_target t={.buffer=buffer,.type=GL_UNSIGNED_BYTE,.format=GL_RGBA8,.bytes=4};for(unsigned j=0;j<4;j++)if(bits[j])t.channels|=1u<<j;
  if(bits[0]==16&&component==GL_UNSIGNED_INT){t.format=GL_RGBA16UI;t.type=GL_UNSIGNED_SHORT;t.bytes=8;}else if(bits[0]==16&&component==GL_INT){t.format=GL_RGBA16I;t.type=GL_SHORT;t.bytes=8;}else if(bits[0]==16&&component==GL_FLOAT){t.format=GL_RGBA16F;t.type=GL_HALF_FLOAT;t.bytes=8;}else if(bits[0]==32&&component==GL_UNSIGNED_INT){t.format=GL_RGBA32UI;t.type=GL_UNSIGNED_INT;t.bytes=16;}else if(bits[0]==32&&component==GL_INT){t.format=GL_RGBA32I;t.type=GL_INT;t.bytes=16;}else if(bits[0]!=8||component!=GL_UNSIGNED_NORMALIZED){note("target_fallback slot=%u red_bits=%d component=%x",i,bits[0],component);report_reasonf("t:bits%d-%x",bits[0],component);return 0;}
  if(encoding!=GL_LINEAR&&encoding!=GL_SRGB){report_reasonf("t:encoding");return 0;}if(i&&encoding==GL_SRGB&&state_enabled(GL_FRAMEBUFFER_SRGB)){report_reasonf("t:mrt-srgb");return 0;}if(!i)c->framebuffer_srgb=encoding==GL_SRGB&&state_enabled(GL_FRAMEBUFFER_SRGB);
  unsigned w=0,h=0;if(!fb){window_size(&w,&h);}else{
   GLint major,minor,kind,object;glGetIntegerv(GL_MAJOR_VERSION,&major);glGetIntegerv(GL_MINOR_VERSION,&minor);int es=es_current();if(es?(major<3||(major==3&&minor<1)):(major<4||(major==4&&minor<5)))return 0;
   glGetFramebufferAttachmentParameteriv(GL_DRAW_FRAMEBUFFER,attachment,GL_FRAMEBUFFER_ATTACHMENT_OBJECT_TYPE,&kind);glGetFramebufferAttachmentParameteriv(GL_DRAW_FRAMEBUFFER,attachment,GL_FRAMEBUFFER_ATTACHMENT_OBJECT_NAME,&object);
   if(kind==GL_TEXTURE){GLint level,layered,tw,th;glGetFramebufferAttachmentParameteriv(GL_DRAW_FRAMEBUFFER,attachment,GL_FRAMEBUFFER_ATTACHMENT_TEXTURE_LEVEL,&level);glGetFramebufferAttachmentParameteriv(GL_DRAW_FRAMEBUFFER,attachment,GL_FRAMEBUFFER_ATTACHMENT_LAYERED,&layered);if(layered){report_reasonf("t:layered");return 0;}tw=th=0;if(es){GLint prior=0;glGetIntegerv(GL_TEXTURE_BINDING_2D,&prior);glBindTexture(GL_TEXTURE_2D,object);glGetTexLevelParameteriv(GL_TEXTURE_2D,level,GL_TEXTURE_WIDTH,&tw);glGetTexLevelParameteriv(GL_TEXTURE_2D,level,GL_TEXTURE_HEIGHT,&th);glBindTexture(GL_TEXTURE_2D,prior);}   /* ES has no DSA; a non-2D texture leaves 0x0 and is refused */
    else{glGetTextureLevelParameteriv(object,level,GL_TEXTURE_WIDTH,&tw);glGetTextureLevelParameteriv(object,level,GL_TEXTURE_HEIGHT,&th);}w=tw;h=th;}
   else if(kind==GL_RENDERBUFFER){GLint prior,tw,th;glGetIntegerv(GL_RENDERBUFFER_BINDING,&prior);glBindRenderbuffer(GL_RENDERBUFFER,object);glGetRenderbufferParameteriv(GL_RENDERBUFFER,GL_RENDERBUFFER_WIDTH,&tw);glGetRenderbufferParameteriv(GL_RENDERBUFFER,GL_RENDERBUFFER_HEIGHT,&th);glBindRenderbuffer(GL_RENDERBUFFER,prior);w=tw;h=th;}else{report_reasonf("t:object-%x",kind);return 0;}
  }
  if(!w||!h||w>LR_GPU_MAX_DIMENSION||h>LR_GPU_MAX_DIMENSION||(i&&(w!=*width||h!=*height))){report_reasonf("t:size-%ux%u",w,h);return 0;}if(!i){*width=w;*height=h;}t.offset=c->color_bytes;t.size=(size_t)w*h*t.bytes;c->color_bytes+=t.size;c->targets[i]=t;c->target_count++;
 }
 if(!c->target_count){report_reasonf("t:no-color");return 0;}for(unsigned i=c->target_count;i<8;i++){GLint b;glGetIntegerv(GL_DRAW_BUFFER0+i,&b);if(b!=GL_NONE)return 0;}return 1;
}
#include "mesa_metal_resident.inc"
#include "guest_fixed_function.inc"
static int gpu_draw(struct bridge_context *c,const struct draw_call *call,GLenum *draw_buffer,unsigned *alpha_bits){
 uint64_t phase=atomic_load_explicit(&report_phases,memory_order_relaxed)?report_now():0;
#define PHASE(slot) do{if(phase){report_add(slot,phase,0);phase=report_now();}}while(0)
 // LR_MESA_METAL_MICROBENCH=1: once per process, the cost of one GL state query and one bridge ioctl.
 {static int done;if(!done&&phase&&config("LR_MESA_METAL_MICROBENCH")&&c->fd>=0){done=1;GLint v;GLfloat m[16];uint64_t t0=report_now();for(int i=0;i<2000;i++)glGetIntegerv(GL_DEPTH_FUNC,&v);uint64_t t1=report_now();for(int i=0;i<2000;i++)(void)glIsEnabled(GL_BLEND);uint64_t t2=report_now();for(int i=0;i<2000;i++)glGetFloatv(GL_MODELVIEW_MATRIX,m);uint64_t t3=report_now();
  GLint vp[4];glGetIntegerv(GL_VIEWPORT,vp);struct lr_gpu_rectangle area={vp[0],vp[1],vp[2],vp[3]};for(int i=0;i<2000;i++){struct lr_gpu_request r={.operation=LR_GPU_VIEWPORT,.address=(uintptr_t)&area,.bytes=sizeof area};call_gpu(c,&r);}uint64_t t4=report_now();
  note("microbench getintegerv_ns=%llu isenabled_ns=%llu getfloatv_matrix_ns=%llu viewport_ioctl_ns=%llu",(unsigned long long)(t1-t0)/2000,(unsigned long long)(t2-t1)/2000,(unsigned long long)(t3-t2)/2000,(unsigned long long)(t4-t3)/2000);phase=report_now();}}
 GLint fb=0,program=0,viewport[4],polygon[2]={GL_FILL,GL_FILL},scissor[4],face,winding;GLboolean mask[4];glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING,&fb);glGetIntegerv(GL_CURRENT_PROGRAM,&program);c->draw_framebuffer=fb;c->last_program=program;c->last_program_label[0]=0;c->last_shader_label[0]=0;if(program&&config("LR_MESA_METAL_TRACE"))glGetObjectLabel(GL_PROGRAM,program,sizeof c->last_program_label,NULL,c->last_program_label);   /* labels only for diagnostics */glGetIntegerv(GL_VIEWPORT,viewport);if(!es_current())glGetIntegerv(GL_POLYGON_MODE,polygon);
 if(fb&&glCheckFramebufferStatus(GL_DRAW_FRAMEBUFFER)!=GL_FRAMEBUFFER_COMPLETE)return draw_rejected(c,call,"framebuffer-incomplete");   /* the window framebuffer is complete while current */
 // Fixed function (no program): an equivalent generated program, resident mode only.
 GLuint ff=0;if(!program){if(!resident_enabled()||ff_disabled())return draw_rejected(c,call,"draw-state");const char *why="ff";uint64_t ff_started=phase?report_now():0;ff=ff_prepare(c,call->mode,&why);if(ff_started)report_add(REPORT_P_FF,ff_started,0);if(!ff){report_reason(why);return draw_rejected(c,call,why);}program=ff;c->last_program=ff;}
 if(!program||state_enabled(GL_COLOR_LOGIC_OP)||glIsEnabled(GL_RASTERIZER_DISCARD)||polygon[0]!=GL_FILL||polygon[1]!=GL_FILL)return draw_rejected(c,call,"draw-state");
 GLint linked=0;glGetProgramiv(program,GL_LINK_STATUS,&linked);if(!linked){note("software_fallback program=%d framebuffer=%d reason=link-failed",program,fb);return draw_rejected(c,call,"program-link-failed");}
 int points=call->mode==GL_POINTS,legacy=call->mode==GL_QUADS||call->mode==GL_QUAD_STRIP||call->mode==GL_POLYGON;   // points: generated (fixed-function) programs only; legacy primitives become triangles
 if(call->count<(points?1:line_mode(call->mode)?2:3)||call->count>LR_GPU_MAX_VERTICES||call->first<0||call->instances<1||call->instances>64||call->base>64||(unsigned)call->instances+call->base>64||call->base_vertex!=0||(points&&!ff)||(legacy&&call->instances!=1)||(call->mode!=GL_TRIANGLES&&call->mode!=GL_TRIANGLE_STRIP&&call->mode!=GL_TRIANGLE_FAN&&!line_mode(call->mode)&&!points&&!legacy)){report_reasonf("dp:mode%x%s%s%s",call->mode,call->count<3?"-short":"",call->instances>64||call->base>64?"-instances":"",call->base_vertex?"-basevertex":"");return draw_rejected(c,call,"draw-parameters");}
 if(line_mode(call->mode)){GLfloat width=1;glGetFloatv(GL_LINE_WIDTH,&width);if(width!=1.0f||state_enabled(GL_LINE_SMOOTH)){report_reasonf("l:width%.2f-smooth%d",width,state_enabled(GL_LINE_SMOOTH));return draw_rejected(c,call,"line-width-or-smooth");}}
 unsigned target_width=0,target_height=0;if(!collect_targets(c,fb,&target_width,&target_height))return draw_rejected(c,call,"targets");*draw_buffer=c->targets[0].buffer;*alpha_bits=c->targets[0].channels&8;
 struct {unsigned width,height;} window={target_width,target_height};
 c->stencil_test=glIsEnabled(GL_STENCIL_TEST);if(c->stencil_test){const char *mode=config("LR_MESA_METAL_MODE");if(mode&&!strcmp(mode,"replace")&&!resident_enabled())return draw_rejected(c,call,"stencil-replace-writeback");if(resident_enabled()&&!fb)return draw_rejected(c,call,"resident-window-stencil");GLint precision=0;glGetFramebufferAttachmentParameteriv(GL_DRAW_FRAMEBUFFER,fb?GL_STENCIL_ATTACHMENT:GL_STENCIL,GL_FRAMEBUFFER_ATTACHMENT_STENCIL_SIZE,&precision);if(precision!=8)return draw_rejected(c,call,"stencil-format");}
 c->depth_test=glIsEnabled(GL_DEPTH_TEST);if(c->depth_test){const char *mode=config("LR_MESA_METAL_MODE");if((!fb&&!resident_enabled())||(mode&&!strcmp(mode,"replace")&&!resident_enabled()))return draw_rejected(c,call,"depth-replace-or-window");/* depth range and polygon offset travel in the raster state (LR_GPU_RASTER_*) */GLint precision,type;GLenum depth_point=fb?GL_DEPTH_ATTACHMENT:GL_DEPTH;glGetFramebufferAttachmentParameteriv(GL_DRAW_FRAMEBUFFER,depth_point,GL_FRAMEBUFFER_ATTACHMENT_DEPTH_SIZE,&precision);glGetFramebufferAttachmentParameteriv(GL_DRAW_FRAMEBUFFER,depth_point,GL_FRAMEBUFFER_ATTACHMENT_COMPONENT_TYPE,&type);c->depth_bits=precision;if(!((precision==32&&(type==GL_FLOAT||type==GL_UNSIGNED_NORMALIZED))||((precision==16||precision==24)&&type==GL_UNSIGNED_NORMALIZED))){note("software_fallback reason=depth-format bits=%d type=%x",precision,type);return draw_rejected(c,call,"depth-format");}}
 for(unsigned i=1;i<c->target_count;i++){if(glIsEnabledi(GL_BLEND,i)!=glIsEnabledi(GL_BLEND,0))return draw_rejected(c,call,"blend-enable-mismatch");GLenum properties[]={GL_BLEND_SRC_RGB,GL_BLEND_DST_RGB,GL_BLEND_SRC_ALPHA,GL_BLEND_DST_ALPHA,GL_BLEND_EQUATION_RGB,GL_BLEND_EQUATION_ALPHA};for(unsigned k=0;k<6;k++){GLint a,b;glGetIntegeri_v(properties[k],0,&a);glGetIntegeri_v(properties[k],i,&b);if(a!=b)return draw_rejected(c,call,"blend-state-mismatch");}}
 GLint samples;glGetIntegerv(GL_SAMPLES,&samples);if(samples)return draw_rejected(c,call,"multisample");
 if(window.width<1||window.height<1||window.width>LR_GPU_MAX_DIMENSION||window.height>LR_GPU_MAX_DIMENSION)return draw_rejected(c,call,"target-dimensions");
 if(c->fd<0){c->fd=open("/dev/linuxrunner-gpu",O_RDWR|O_CLOEXEC);if(c->fd<0)return draw_rejected(c,call,"device-open");struct lr_gpu_request r={.operation=LR_GPU_CREATE,.width=window.width,.height=window.height};if(!call_gpu(c,&r)){close(c->fd);c->fd=-1;return draw_rejected(c,call,"device-create");}c->width=window.width;c->height=window.height;note("Metal_companion context_owned=1 GL_VERSION=%s GL_RENDERER=%s",glGetString(GL_VERSION),glGetString(GL_RENDERER));}
 if(c->width!=(unsigned)window.width||c->height!=(unsigned)window.height){struct lr_gpu_request r={.operation=LR_GPU_RESIZE,.width=window.width,.height=window.height};if(!call_gpu(c,&r))return draw_rejected(c,call,"device-resize");c->width=window.width;c->height=window.height;}
 c->draw_framebuffer=fb;c->pixel_format=c->targets[0].format;c->pixel_type=c->targets[0].type;c->pixel_bytes=c->targets[0].bytes;c->channels=c->targets[0].channels;
 size_t bytes=c->color_bytes;if(bytes>c->pixel_capacity){unsigned char *before=malloc(bytes),*after=malloc(bytes),*oracle=malloc(bytes);if(!before||!after||!oracle){free(before);free(after);free(oracle);return 0;}free(c->before);free(c->after);free(c->oracle);c->before=before;c->after=after;c->oracle=oracle;c->pixel_capacity=bytes;}
 if(c->depth_test){size_t needed=(size_t)c->width*c->height*4;if(c->depth_capacity<needed){float *before=malloc(needed),*after=malloc(needed),*oracle=malloc(needed);if(!before||!after||!oracle){free(before);free(after);free(oracle);return 0;}free(c->depth_before);free(c->depth_after);free(c->depth_oracle);c->depth_before=before;c->depth_after=after;c->depth_oracle=oracle;c->depth_capacity=needed;}}
 if(c->stencil_test){size_t needed=(size_t)c->width*c->height;if(c->stencil_capacity<needed){unsigned char *before=malloc(needed),*after=malloc(needed),*oracle=malloc(needed);if(!before||!after||!oracle){free(before);free(after);free(oracle);return 0;}free(c->stencil_before);free(c->stencil_after);free(c->stencil_oracle);c->stencil_before=before;c->stencil_after=after;c->stencil_oracle=oracle;c->stencil_capacity=needed;}}
 struct resident_targets resident_bound={0};int resident=resident_enabled();PHASE(REPORT_P_STATE);
 if(resident){
  // GPU-resident targets: no import; Metal copies are uploaded only when stale.
  unsigned masks[4]={0};for(unsigned i=0;i<c->target_count;i++){GLboolean m[4];glGetBooleani_v(GL_COLOR_WRITEMASK,i,m);masks[i]=(m[0]|(m[1]<<1)|(m[2]<<2)|(m[3]<<3))&c->targets[i].channels;}
  if(!resident_targets(c,fb,c->width,c->height,c->depth_test,c->stencil_test,&resident_bound)||!resident_bind(c,&resident_bound,masks))return draw_rejected(c,call,"resident-targets");
 }else{
 size_t import_bytes=sizeof(struct lr_gpu_mrt)+bytes;struct lr_gpu_mrt *import=calloc(1,import_bytes);if(!import)return 0;import->count=c->target_count;
 for(unsigned i=0;i<c->target_count;i++){struct color_target t=c->targets[i];read_color(c,t.buffer,c->before+t.offset);memcpy((char *)import+sizeof *import+t.offset,c->before+t.offset,t.size);GLboolean masks[4];glGetBooleani_v(GL_COLOR_WRITEMASK,i,masks);import->color[i].format=t.format;import->color[i].mask=(masks[0]|(masks[1]<<1)|(masks[2]<<2)|(masks[3]<<3))&t.channels;import->color[i].offset=sizeof *import+t.offset;import->color[i].bytes=t.size;}
 struct lr_gpu_request seeded={.operation=LR_GPU_IMPORT_MRT,.width=c->width,.height=c->height,.address=(uintptr_t)import,.bytes=import_bytes};int imported=call_gpu(c,&seeded);free(import);if(!imported)return 0;
 }

 PHASE(REPORT_P_TARGETS);unsigned slot=0;struct gpu_program *p=compile_program(c,program,&slot);if(!p)return draw_rejected(c,call,"program");c->last_program=program;if(config("LR_MESA_METAL_TRACE"))snprintf(c->last_shader_label,sizeof c->last_shader_label,"%s",p->shader_label);PHASE(REPORT_P_LOOKUP);
 uint32_t source[LR_GPU_MAX_VERTICES],indices[LR_GPU_MAX_VERTICES*3];unsigned native_vertices=0,index_count=0;
 unsigned vertex_base=0;
 if(call->kind<3||call->kind==8){if((unsigned)call->count>LR_GPU_MAX_VERTICES)return draw_rejected(c,call,"array-range");vertex_base=(unsigned)call->first;for(int i=0;i<call->count;i++)source[i]=i;native_vertices=call->count;}
 else{GLint element;glGetIntegerv(GL_ELEMENT_ARRAY_BUFFER_BINDING,&element);unsigned cb=call->type==GL_UNSIGNED_BYTE?1:call->type==GL_UNSIGNED_SHORT?2:call->type==GL_UNSIGNED_INT?4:0;_Alignas(4) unsigned char raw[LR_GPU_MAX_VERTICES*4];if(!cb||(element?!read_buffer_bytes(element,(uintptr_t)call->indices,(size_t)call->count*cb,raw):(!call->indices||(memcpy(raw,call->indices,(size_t)call->count*cb),0))))return draw_rejected(c,call,"index-buffer");/* no element buffer: client-memory indices (compatibility contexts) */uint32_t lowest=UINT32_MAX,highest=0;
  // Typed loads (a variable-size memcpy per index is a library call per element).
  if(cb==2){const uint16_t *x=(const void *)raw;for(int i=0;i<call->count;i++)source[i]=x[i];}else if(cb==4){const uint32_t *x=(const void *)raw;for(int i=0;i<call->count;i++)source[i]=x[i];}else for(int i=0;i<call->count;i++)source[i]=raw[i];
  for(int i=0;i<call->count;i++){uint32_t v=source[i];if(v<lowest)lowest=v;if(v>highest)highest=v;}
  if(call->count&&highest-lowest>=LR_GPU_MAX_VERTICES)return draw_rejected(c,call,"index-range");vertex_base=call->count?lowest:0;   /* base_vertex is 0 here (rejected above otherwise) */
  native_vertices=call->count?highest-lowest+1:0;if(vertex_base)for(int i=0;i<call->count;i++)source[i]-=vertex_base;}
 int lines=line_mode(call->mode);GLint provoking=GL_LAST_VERTEX_CONVENTION;if(!es_current()||es_extension("GL_EXT_provoking_vertex"))glGetIntegerv(GL_PROVOKING_VERTEX,&provoking);int rotated=0;
 if(call->mode==GL_TRIANGLES&&provoking==GL_LAST_VERTEX_CONVENTION){if(call->count%3)return draw_rejected(c,call,"triangle-count");for(int i=0;i<call->count;i+=3){indices[index_count++]=source[i+2];indices[index_count++]=source[i];indices[index_count++]=source[i+1];}rotated=1;}   // rotated below otherwise: Metal flat-shades from the first vertex
 else if(call->mode==GL_LINES){for(int i=0;i+1<call->count;i+=2){indices[index_count++]=source[i];indices[index_count++]=source[i+1];}}   // a trailing odd vertex is ignored, as in GL
 else if(lines){int n=call->count;for(int i=1;i<n;i++){indices[index_count++]=source[i-1];indices[index_count++]=source[i];}if(call->mode==GL_LINE_LOOP){indices[index_count++]=source[n-1];indices[index_count++]=source[0];}}
 else if(points){for(int i=0;i<call->count;i++)indices[index_count++]=source[i];}
 else if(call->mode==GL_QUADS||call->mode==GL_QUAD_STRIP){
  // Quad P0..P3 in polygon order (QUADS: i..i+3; QUAD_STRIP: 2i,2i+1,2i+3,2i+2). GL
  // flat-shades it from its provoking vertex Pp (QUADS: P3; QUAD_STRIP: P2, i.e.
  // vertex 2i+3): split along the diagonal through Pp so both triangles end with
  // it (the rotation below then makes it Metal's first vertex).
  int step=call->mode==GL_QUADS?4:2,pv=call->mode==GL_QUADS?3:2;
  for(int i=0;i+3<call->count;i+=step){uint32_t q[4];if(call->mode==GL_QUADS){q[0]=source[i];q[1]=source[i+1];q[2]=source[i+2];q[3]=source[i+3];}else{q[0]=source[i];q[1]=source[i+1];q[2]=source[i+3];q[3]=source[i+2];}
   indices[index_count++]=q[(pv+1)%4];indices[index_count++]=q[(pv+2)%4];indices[index_count++]=q[pv];
   indices[index_count++]=q[(pv+2)%4];indices[index_count++]=q[(pv+3)%4];indices[index_count++]=q[pv];}}
 else if(call->mode==GL_POLYGON){GLint shade=GL_SMOOTH;glGetIntegerv(GL_SHADE_MODEL,&shade);if(shade==GL_FLAT)return draw_rejected(c,call,"flat-polygon");for(int i=2;i<call->count;i++){indices[index_count++]=source[0];indices[index_count++]=source[i-1];indices[index_count++]=source[i];}}
 else if(call->mode==GL_TRIANGLES){if(call->count%3)return draw_rejected(c,call,"triangle-count");for(int i=0;i<call->count;i++)indices[index_count++]=source[i];}
 else if(call->mode==GL_TRIANGLE_STRIP||call->mode==GL_TRIANGLE_FAN)for(int i=2;i<call->count;i++){indices[index_count++]=source[call->mode==GL_TRIANGLE_FAN?0:(i%2?i-1:i-2)];indices[index_count++]=source[call->mode==GL_TRIANGLE_FAN?i-1:(i%2?i-2:i-1)];indices[index_count++]=source[i];}
 if(provoking==GL_LAST_VERTEX_CONVENTION&&!points&&!rotated){if(lines)for(unsigned i=0;i<index_count;i+=2){uint32_t last=indices[i+1];indices[i+1]=indices[i];indices[i]=last;}else for(unsigned i=0;i<index_count;i+=3){uint32_t last=indices[i+2];indices[i+2]=indices[i+1];indices[i+1]=indices[i];indices[i]=last;}}   // Metal flat-shades from the first vertex
 if(index_count>LR_GPU_MAX_VERTICES)return draw_rejected(c,call,"index-count");PHASE(REPORT_P_INDICES);
 size_t packet_bytes=sizeof(struct lr_gl_draw)+(size_t)native_vertices*sizeof(struct lr_gl_vertex)+(size_t)index_count*4+4;struct lr_gl_draw *draw=calloc(1,packet_bytes);struct lr_gpu_instance_attributes *inst=calloc(1,sizeof *inst);if(!draw||!inst){free(draw);free(inst);return 0;}draw->program=slot+1;draw->count=native_vertices;int ok=0;
 PHASE(REPORT_P_PROGRAM);if(ff){ff_block_program=ff;ff_block_data=&ff_block_current;ff_block_bytes=sizeof ff_block_current;}uniform_targets=resident?&resident_bound:NULL;int uniforms_ok=uniforms(c,p,slot,draw);uniform_targets=NULL;ff_block_program=0;ff_block_data=NULL;PHASE(REPORT_P_UNIFORMS);if(!uniforms_ok){if(config("LR_MESA_METAL_TRACE"))fprintf(stderr,"MESA_METAL_FALLBACK context_id=%llu program=%u label=%s reason=uniforms\n",(unsigned long long)c->id,program,c->last_program_label);report_reason("uniforms");goto done;}draw_vertex_base=vertex_base;int fetched=ff?(*inst=(struct lr_gpu_instance_attributes){.instances=call->instances+call->base},ff_vertices(draw)):vertices(p,draw,call->instances,call->base,inst);draw_vertex_base=0;if(!fetched){if(config("LR_MESA_METAL_TRACE"))fprintf(stderr,"MESA_METAL_FALLBACK context_id=%llu program=%u label=%s reason=vertices\n",(unsigned long long)c->id,program,c->last_program_label);report_reason("vertices");goto done;}
 PHASE(REPORT_P_VERTICES);struct lr_gpu_request r={.operation=LR_GPU_INSTANCE_ATTRIBUTES,.flags=LR_GPU_TYPED_DATA_BIT,.address=(uintptr_t)inst,.bytes=sizeof *inst};if(!call_gpu(c,&r))goto done;
 struct lr_gpu_rectangle area={viewport[0],viewport[1],viewport[2],viewport[3]};r=(struct lr_gpu_request){.operation=LR_GPU_VIEWPORT,.address=(uintptr_t)&area,.bytes=sizeof area};if(!call_gpu(c,&r))goto done;
 glGetIntegerv(GL_SCISSOR_BOX,scissor);glGetIntegerv(GL_CULL_FACE_MODE,&face);glGetIntegerv(GL_FRONT_FACE,&winding);glGetBooleanv(GL_COLOR_WRITEMASK,mask);struct lr_gpu_raster_state raster={.scissor=glIsEnabled(GL_SCISSOR_TEST),.color_mask=(mask[0]|(mask[1]<<1)|(mask[2]<<2)|(mask[3]<<3))&c->channels,.cull_face=face,.front_face=winding,.x=scissor[0],.y=scissor[1],.width=scissor[2],.height=scissor[3],.framebuffer_srgb=c->framebuffer_srgb,.depth_bits=c->depth_test?c->depth_bits:0};
 if(c->depth_test){GLdouble range[2];glGetDoublev(GL_DEPTH_RANGE,range);if(range[0]!=0||range[1]!=1){raster.extensions|=LR_GPU_RASTER_DEPTH_RANGE;raster.depth_near=(float)range[0];raster.depth_far=(float)range[1];}
  if(glIsEnabled(GL_POLYGON_OFFSET_FILL)&&!lines&&!points){raster.extensions|=LR_GPU_RASTER_POLYGON_OFFSET;glGetFloatv(GL_POLYGON_OFFSET_FACTOR,&raster.offset_factor);glGetFloatv(GL_POLYGON_OFFSET_UNITS,&raster.offset_units);}}r=(struct lr_gpu_request){.operation=LR_GPU_RASTER_STATE,.address=(uintptr_t)&raster,.bytes=sizeof raster};if(!call_gpu(c,&r))goto done;
 struct lr_gpu_stencil_state stencil={.enabled=c->stencil_test};stencil.front=stencil.back=(struct lr_gpu_stencil_face){.function=GL_ALWAYS,.read_mask=255,.write_mask=255,.fail=GL_KEEP,.depth_fail=GL_KEEP,.depth_pass=GL_KEEP};
 if(c->stencil_test){GLenum front[]={GL_STENCIL_FUNC,GL_STENCIL_REF,GL_STENCIL_VALUE_MASK,GL_STENCIL_WRITEMASK,GL_STENCIL_FAIL,GL_STENCIL_PASS_DEPTH_FAIL,GL_STENCIL_PASS_DEPTH_PASS},back[]={GL_STENCIL_BACK_FUNC,GL_STENCIL_BACK_REF,GL_STENCIL_BACK_VALUE_MASK,GL_STENCIL_BACK_WRITEMASK,GL_STENCIL_BACK_FAIL,GL_STENCIL_BACK_PASS_DEPTH_FAIL,GL_STENCIL_BACK_PASS_DEPTH_PASS};uint32_t *f=(uint32_t *)&stencil.front,*b=(uint32_t *)&stencil.back;for(unsigned i=0;i<7;i++){GLint value;glGetIntegerv(front[i],&value);f[i]=(i==2||i==3)?value&255:value;glGetIntegerv(back[i],&value);b[i]=(i==2||i==3)?value&255:value;}
  if(!resident){read_stencil(c,c->stencil_before);r=(struct lr_gpu_request){.operation=LR_GPU_IMPORT_STENCIL,.width=c->width,.height=c->height,.address=(uintptr_t)c->stencil_before,.bytes=(size_t)c->width*c->height};if(!call_gpu(c,&r))goto done;}}
 r=(struct lr_gpu_request){.operation=LR_GPU_STENCIL_STATE,.address=(uintptr_t)&stencil,.bytes=sizeof stencil};if(!call_gpu(c,&r))goto done;
 struct lr_gpu_blend blend={.enabled=glIsEnabled(GL_BLEND)};glGetIntegerv(GL_BLEND_SRC_RGB,(GLint *)&blend.src_rgb);glGetIntegerv(GL_BLEND_DST_RGB,(GLint *)&blend.dst_rgb);glGetIntegerv(GL_BLEND_SRC_ALPHA,(GLint *)&blend.src_alpha);glGetIntegerv(GL_BLEND_DST_ALPHA,(GLint *)&blend.dst_alpha);glGetIntegerv(GL_BLEND_EQUATION_RGB,(GLint *)&blend.equation_rgb);glGetIntegerv(GL_BLEND_EQUATION_ALPHA,(GLint *)&blend.equation_alpha);glGetFloatv(GL_BLEND_COLOR,blend.color);r=(struct lr_gpu_request){.operation=LR_GPU_BLEND,.address=(uintptr_t)&blend,.bytes=sizeof blend};if(!call_gpu(c,&r))goto done;
 
 if(c->depth_test){if(!resident){read_depth(c,c->depth_before);r=(struct lr_gpu_request){.operation=LR_GPU_IMPORT_DEPTH,.width=c->width,.height=c->height,.address=(uintptr_t)c->depth_before,.bytes=(size_t)c->width*c->height*4};if(!call_gpu(c,&r))goto done;}GLint function;GLboolean write;glGetIntegerv(GL_DEPTH_FUNC,&function);glGetBooleanv(GL_DEPTH_WRITEMASK,&write);struct lr_gpu_depth_state state={.function=function,.write_mask=write,.clear_depth=1};r=(struct lr_gpu_request){.operation=LR_GPU_DEPTH_STATE,.address=(uintptr_t)&state,.bytes=sizeof state};if(!call_gpu(c,&r))goto done;}
 PHASE(REPORT_P_STATE_IOCTL);size_t at=sizeof *draw+(size_t)native_vertices*sizeof(struct lr_gl_vertex);memcpy((char *)draw+at,indices,index_count*4);memcpy((char *)draw+at+index_count*4,&call->base,4);
 r=(struct lr_gpu_request){.operation=LR_GPU_DRAW_INDEXED_BASE,.width=call->instances,.height=index_count,.flags=LR_GPU_TYPED_DATA_BIT|(c->depth_test?LR_GPU_DEPTH_TEST_BIT:0)|(points?LR_GPU_POINTS_BIT:lines?LR_GPU_LINES_BIT:glIsEnabled(GL_CULL_FACE)?LR_GPU_CULL_BIT:0),.count=native_vertices,.address=(uintptr_t)draw,.bytes=packet_bytes};if(!call_gpu(c,&r))goto done;PHASE(REPORT_P_DRAW_IOCTL);
 if(resident){
  // Metal now holds the newest contents of every bound target; Mesa is behind.
  for(unsigned i=0;i<resident_bound.count;i++)resident_metal_wrote(c,resident_bound.color[i]);
  GLboolean depth_write=GL_FALSE;if(c->depth_test)glGetBooleanv(GL_DEPTH_WRITEMASK,&depth_write);
  if(resident_bound.depth&&depth_write)resident_metal_wrote(c,resident_bound.depth);
  if(resident_bound.stencil)resident_metal_wrote(c,resident_bound.stencil);
  resident_state(c)->draws++;ok=1;goto done;}
 ok=1;for(unsigned i=0;i<c->target_count;i++){struct color_target t=c->targets[i];r=(struct lr_gpu_request){.operation=LR_GPU_READ_MRT,.count=i,.flags=t.format,.width=c->width,.height=c->height,.address=(uintptr_t)(c->after+t.offset),.bytes=t.size};if(!call_gpu(c,&r)){ok=0;break;}}if(ok&&c->depth_test){r=(struct lr_gpu_request){.operation=LR_GPU_READ_DEPTH,.width=c->width,.height=c->height,.address=(uintptr_t)c->depth_after,.bytes=(size_t)c->width*c->height*4};ok=call_gpu(c,&r);}
 if(ok&&c->stencil_test){r=(struct lr_gpu_request){.operation=LR_GPU_READ_STENCIL,.width=c->width,.height=c->height,.address=(uintptr_t)c->stencil_after,.bytes=(size_t)c->width*c->height};ok=call_gpu(c,&r);}
 done:free(draw);free(inst);return ok;
}
static float half_float(uint16_t h){unsigned sign=(h&0x8000u)<<16,e=(h>>10)&31,m=h&1023,bits;if(!e){if(!m)bits=sign;else{int exponent=-14;while(!(m&1024)){m<<=1;exponent--;}bits=sign|((unsigned)(exponent+127)<<23)|((m&1023)<<13);}}else bits=sign|((e==31?255:e+112)<<23)|(m<<13);float f;memcpy(&f,&bits,4);return f;}
// Verification compares whole pixels of one colour target with the same
// per-format tolerances as before.
static int pixels_match(const struct color_target *t,const unsigned char *a,size_t pa,const unsigned char *b,size_t pb){
 for(unsigned ch=0;ch<4;ch++){if(!(t->channels&(1u<<ch)))continue;size_t ia=pa*4+ch,ib=pb*4+ch;
  if(t->type==GL_UNSIGNED_BYTE){if(abs((int)a[t->offset+ia]-(int)b[t->offset+ib])>2)return 0;}
  else if(t->type==GL_HALF_FLOAT){const uint16_t *x=(const void *)(a+t->offset),*y=(const void *)(b+t->offset);if(x[ia]!=y[ib]){float u=half_float(x[ia]),v=half_float(y[ib]);if(!isfinite(u)||!isfinite(v)||fabsf(u-v)>0.002f)return 0;}}
  else{unsigned cb=t->bytes/4;if(memcmp(a+t->offset+ia*cb,b+t->offset+ib*cb,cb))return 0;}}
 return 1;
}
// GL leaves exact rasterization implementation-dependent: which of two
// triangles sharing an edge covers a pixel on it, and which of two surfaces at
// equal depth along a shared edge passes a LEQUAL test. A differing pixel is
// such an edge tie when
//  - the Metal value occurs among Mesa's 3x3 neighbours and the Mesa value
//    among Metal's (same surfaces, split differently along an interior edge), or
//  - either renderer covered the pixel and a pixel within two of it was
//    covered by neither: at a silhouette, coverage of edge pixels differs, and
//    a front face and an unculled back face sharing the silhouette edge tie in
//    depth (over two pixels where the face is nearly edge-on).
// Any other difference is a real mismatch.
static int edge_tie(const struct color_target *t,const unsigned char *metal,const unsigned char *mesa,const unsigned char *before,size_t px,unsigned width,unsigned height){
 unsigned x=px%width,y=px/width;int metal_near=0,mesa_near=0,silhouette=0;
 int covered=memcmp(metal+t->offset+px*t->bytes,before+t->offset+px*t->bytes,t->bytes)||memcmp(mesa+t->offset+px*t->bytes,before+t->offset+px*t->bytes,t->bytes);
 for(int dy=-2;dy<=2;dy++)for(int dx=-2;dx<=2;dx++){if(!dx&&!dy)continue;long nx=(long)x+dx,ny=(long)y+dy;if(nx<0||ny<0||nx>=width||ny>=height)continue;size_t q=(size_t)ny*width+nx;
  if(dx>=-1&&dx<=1&&dy>=-1&&dy<=1){if(pixels_match(t,metal,px,mesa,q))metal_near=1;if(pixels_match(t,mesa,px,metal,q))mesa_near=1;}
  if(covered&&!memcmp(metal+t->offset+q*t->bytes,before+t->offset+q*t->bytes,t->bytes)&&!memcmp(mesa+t->offset+q*t->bytes,before+t->offset+q*t->bytes,t->bytes))silhouette=1;}
 return (metal_near&&mesa_near)||silhouette;
}
static int depth_edge_tie(const float *metal,const float *mesa,size_t px,unsigned width,unsigned height){
 unsigned x=px%width,y=px/width;int metal_near=0,mesa_near=0;
 for(int dy=-1;dy<=1;dy++)for(int dx=-1;dx<=1;dx++){if(!dx&&!dy)continue;long nx=(long)x+dx,ny=(long)y+dy;if(nx<0||ny<0||nx>=width||ny>=height)continue;size_t q=(size_t)ny*width+nx;
  if(fabsf(metal[px]-mesa[q])<=1e-6f)metal_near=1;if(fabsf(mesa[px]-metal[q])<=1e-6f)mesa_near=1;}
 return metal_near&&mesa_near;
}
// Independent, opt-in budget: UI sampler failures must not hide scene draws.
static void trace_draw(struct bridge_context *c,const struct draw_call *d){
 if(!config("LR_MESA_METAL_TRACE")||!c||c->traced>=128)return;GLint program=0,fb=0,ebo=0,depth_func=0;glGetIntegerv(GL_CURRENT_PROGRAM,&program);for(unsigned i=0;i<c->traced;i++)if(c->traced_program[i]==(GLuint)program)return;c->traced_program[c->traced++]=program;
 char label[256]={0};if(program)glGetObjectLabel(GL_PROGRAM,program,sizeof label,NULL,label);glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING,&fb);glGetIntegerv(GL_ELEMENT_ARRAY_BUFFER_BINDING,&ebo);glGetIntegerv(GL_DEPTH_FUNC,&depth_func);
 fprintf(stderr,"MESA_METAL_TRACE context=%llu program=%d label=%s kind=%u mode=%x count=%d instances=%d base=%u base_vertex=%d index_type=%x ebo=%d framebuffer=%d depth=%d depth_func=%x stencil=%d\n",(unsigned long long)c->id,program,label,d->kind,d->mode,d->count,d->instances,d->base,d->base_vertex,d->type,ebo,fb,glIsEnabled(GL_DEPTH_TEST),depth_func,glIsEnabled(GL_STENCIL_TEST));
 GLint attributes=0;if(!program)return;glGetProgramiv(program,GL_ACTIVE_ATTRIBUTES,&attributes);for(GLint i=0;i<attributes&&i<LR_GL_MAX_ATTRIBUTES;i++){char name[256];GLint size;GLenum type;glGetActiveAttrib(program,i,sizeof name,NULL,&size,&type,name);GLint location=glGetAttribLocation(program,name),buffer=0;if(location>=0)glGetVertexAttribiv(location,GL_VERTEX_ATTRIB_ARRAY_BUFFER_BINDING,&buffer);GLint divisor=0;if(location>=0)glGetVertexAttribiv(location,GL_VERTEX_ATTRIB_ARRAY_DIVISOR,&divisor);fprintf(stderr,"MESA_METAL_TRACE_ATTRIBUTE program=%d name=%s location=%d type=%x size=%d buffer=%d divisor=%d\n",program,name,location,type,size,buffer,divisor);}
}
// Triangle and line draws larger than one packet run as consecutive chunks (each
// chunk may still fall back to Mesa on its own; order is preserved).
static void dispatch_draw(const struct draw_call *draw);
static unsigned index_bytes(GLenum type);
static int split_draw(const struct draw_call *d){
 if(!resident_enabled()||d->instances!=1||d->base||d->base_vertex||(d->kind!=0&&d->kind!=3)||(d->mode!=GL_TRIANGLES&&d->mode!=GL_LINES)||d->count<=(GLsizei)LR_GPU_MAX_VERTICES)return 0;
 unsigned per=d->mode==GL_TRIANGLES?3:2,cb=d->kind==3?index_bytes(d->type):0;GLsizei chunk=(GLsizei)((LR_GPU_MAX_VERTICES-1)/per*per);if(d->kind==3&&!cb)return 0;
 for(GLsizei at=0;at<d->count;at+=chunk){struct draw_call part=*d;part.count=d->count-at<chunk?d->count-at:chunk;if(d->kind==0)part.first=d->first+at;else part.indices=(const char *)d->indices+(size_t)at*cb;dispatch_draw(&part);}
 return 1;
}
static void dispatch_draw(const struct draw_call *draw){if(atomic_load_explicit(&bridge_retired,memory_order_relaxed)){mesa_draw(draw);return;}if(split_draw(draw))return;const char *mode=config("LR_MESA_METAL_MODE");struct bridge_context *trace_context=NULL;if(config("LR_MESA_METAL_TRACE")){trace_context=current_context();trace_draw(trace_context,draw);}if(!mode||!strcmp(mode,"off")){mesa_draw(draw);return;}struct bridge_context *c=trace_context?trace_context:current_context();const char *filter=config("LR_MESA_METAL_PROGRAM_FILTER");if(filter&&*filter){GLint program=0;char label[256]={0};glGetIntegerv(GL_CURRENT_PROGRAM,&program);if(program)glGetObjectLabel(GL_PROGRAM,program,sizeof label,NULL,label);if(!strstr(label,filter)){if(c)c->fallback++;mesa_draw(draw);return;}}unsigned limit=draw_limit();if(!c||c->owner!=current_tid()||c->attempted>=limit){if(c){report_reason(c->attempted>=limit?"budget":"foreign-thread");if(c->attempted>=limit&&!c->budget_noted){GLint program=0;glGetIntegerv(GL_CURRENT_PROGRAM,&program);c->budget_noted=1;note("draw_budget_exhausted context_id=%llu requested=%u committed=%llu program=%d",(unsigned long long)c->id,limit,(unsigned long long)c->committed,program);}c->fallback++;}mesa_draw(draw);return;}
 GLenum buffer;unsigned alpha;int own_errors=c->resident||resident_enabled()||es_current();GLenum resident_pending=own_errors?resident_errors_begin():GL_NO_ERROR;resident_reentry++;uint64_t draw_started=atomic_load_explicit(&report_phases,memory_order_relaxed)?report_now():0;int ok=gpu_draw(c,draw,&buffer,&alpha);resident_reentry--;if(ok)report_add(REPORT_GPU_DRAW,draw_started,0);if(own_errors)resident_errors_end(c,resident_pending);if(!ok){c->fallback++;mesa_draw(draw);return;}c->attempted++;
 if(c->resident&&resident_enabled()){c->committed++;return;}
 int verify=strcmp(mode,"replace")!=0;size_t mismatches=0,changed=0,edge_ties=0;
 // LR_MESA_METAL_TRACE: where the first few differences are, and by how much.
 unsigned details=config("LR_MESA_METAL_TRACE")?4:0;
#define MISMATCH_DETAIL(kind,target,index,metal,mesa) do{if(details){details--;size_t px=(index)/4,ch=(index)%4;note("mismatch_detail program=%u kind=%s target=%u x=%zu y=%zu channel=%zu metal=%g mesa=%g",c->last_program,kind,(unsigned)(target),px%c->width,px/c->width,ch,(double)(metal),(double)(mesa));}}while(0)
 if(verify)mesa_draw(draw);
 GLenum verify_pending=own_errors?resident_errors_begin():GL_NO_ERROR;   /* the oracle draw's errors are the application's */
 for(unsigned target=0;target<c->target_count;target++){struct color_target t=c->targets[target];
  if(verify){read_color(c,t.buffer,c->oracle+t.offset);for(size_t px=0;px<(size_t)c->width*c->height;px++){if(pixels_match(&t,c->after,px,c->oracle,px))continue;
   if(edge_tie(&t,c->after,c->oracle,c->before,px,c->width,c->height)){edge_ties++;continue;}
   mismatches++;if(details){details--;float mv[4]={0},ov[4]={0},bv[4]={0};if(t.type==GL_HALF_FLOAT){const uint16_t *m=(const void *)(c->after+t.offset),*o=(const void *)(c->oracle+t.offset),*b=(const void *)(c->before+t.offset);for(int k=0;k<4;k++){mv[k]=half_float(m[px*4+k]);ov[k]=half_float(o[px*4+k]);bv[k]=half_float(b[px*4+k]);}}else if(t.type==GL_UNSIGNED_BYTE){for(int k=0;k<4;k++){mv[k]=c->after[t.offset+px*4+k];ov[k]=c->oracle[t.offset+px*4+k];bv[k]=c->before[t.offset+px*4+k];}}
     note("mismatch_detail program=%u target=%u x=%zu y=%zu format=%x metal=%g,%g,%g,%g mesa=%g,%g,%g,%g before=%g,%g,%g,%g",c->last_program,target,px%c->width,px/c->width,t.format,mv[0],mv[1],mv[2],mv[3],ov[0],ov[1],ov[2],ov[3],bv[0],bv[1],bv[2],bv[3]);}}}
  for(size_t i=0;i<(size_t)c->width*c->height;i++)if(memcmp(c->before+t.offset+i*t.bytes,c->after+t.offset+i*t.bytes,t.bytes))changed++;
 }
 if(verify&&c->depth_test){read_depth(c,c->depth_oracle);for(size_t i=0;i<(size_t)c->width*c->height;i++)if(fabsf(c->depth_after[i]-c->depth_oracle[i])>1e-6f){if(depth_edge_tie(c->depth_after,c->depth_oracle,i,c->width,c->height)){edge_ties++;continue;}mismatches++;MISMATCH_DETAIL("depth",0,i*4,c->depth_after[i],c->depth_oracle[i]);}}
 if(verify&&c->stencil_test){read_stencil(c,c->stencil_oracle);for(size_t i=0;i<(size_t)c->width*c->height;i++)if(c->stencil_after[i]!=c->stencil_oracle[i]){mismatches++;MISMATCH_DETAIL("stencil",0,i*4,c->stencil_after[i],c->stencil_oracle[i]);}}
#undef MISMATCH_DETAIL
 // Edge ties are bounded: more than 1% of the pixels this draw changed is
 // not an edge effect.
 if(edge_ties&&edge_ties*100>changed+1){note("edge_ties_excessive ties=%zu changed=%zu keeping=Mesa",edge_ties,changed);mismatches+=edge_ties;}
 else if(edge_ties&&details)note("edge_ties program=%u ties=%zu changed=%zu",c->last_program,edge_ties,changed);
 if(mismatches){c->mismatched++;note("pixel_mismatch program_draw=1 differences=%zu keeping=Mesa",mismatches);
  // LR_MESA_METAL_DUMP_DIR: raw Metal/Mesa colour targets of the first mismatching draw, for offline analysis.
  static int dumped;const char *dir=config("LR_MESA_METAL_DUMP_DIR");if(dir&&!dumped){dumped=1;for(unsigned target=0;target<c->target_count;target++){struct color_target t=c->targets[target];char path[512];
   for(int which=0;which<2;which++){snprintf(path,sizeof path,"%s/target%u-%s-%ux%u-%x.raw",dir,target,which?"mesa":"metal",c->width,c->height,t.format);FILE *f=fopen(path,"wb");if(f){fwrite((which?c->oracle:c->after)+t.offset,1,t.size,f);fclose(f);}}}}
  if(own_errors)resident_errors_end(c,verify_pending);return;}if(verify)c->verified++;
 for(unsigned i=0;i<c->target_count;i++)present_color(c,c->targets[i]);c->committed++;struct lr_gpu_request r={.operation=LR_GPU_INFO};if(call_gpu(c,&r))fprintf(stderr,"MESA_METAL_GPU_PRESENT pid=%ld context_id=%llu draw=%llu verified=%d changed_pixels=%zu Metal_submissions=%llu gpu_ns=%llu framebuffer=%u format=%x targets=%u depth=%u stencil=%u budget=%u program=%u program_label=%s shader_label=%s mode=%x count=%d first=%d instances=%d kind=%u index_type=%x base=%u base_vertex=%d context=hybrid_Mesa_Core_GL_Metal_draws\n",(long)getpid(),(unsigned long long)c->id,(unsigned long long)c->committed,verify,changed,(unsigned long long)r.submissions,(unsigned long long)r.gpu_nanoseconds,c->draw_framebuffer,c->pixel_format,c->target_count,c->depth_test,c->stencil_test,draw_limit(),c->last_program,c->last_program_label,c->last_shader_label,draw->mode,draw->count,draw->first,draw->instances,draw->kind,draw->type,draw->base,draw->base_vertex);
 if(own_errors)resident_errors_end(c,verify_pending);
}
void glDrawArrays(GLenum m,GLint first,GLsizei n){dispatch_draw(&(struct draw_call){.mode=m,.first=first,.count=n,.instances=1,.kind=0});}
void glDrawArraysInstanced(GLenum m,GLint first,GLsizei n,GLsizei instances){dispatch_draw(&(struct draw_call){.mode=m,.first=first,.count=n,.instances=instances,.kind=1});}
void glDrawArraysInstancedBaseInstance(GLenum m,GLint first,GLsizei n,GLsizei instances,GLuint base){dispatch_draw(&(struct draw_call){.mode=m,.first=first,.count=n,.instances=instances,.base=base,.kind=2});}
void glDrawElements(GLenum m,GLsizei n,GLenum type,const void *indices){dispatch_draw(&(struct draw_call){.mode=m,.count=n,.instances=1,.type=type,.indices=indices,.kind=3});}
void glDrawRangeElements(GLenum m,GLuint start,GLuint end,GLsizei n,GLenum type,const void *indices){if(end<start){typeof(&glDrawRangeElements) f=original("glDrawRangeElements");if(f)f(m,start,end,n,type,indices);return;}dispatch_draw(&(struct draw_call){.mode=m,.count=n,.instances=1,.type=type,.indices=indices,.range_start=start,.range_end=end,.kind=10});}
void glDrawElementsInstanced(GLenum m,GLsizei n,GLenum type,const void *indices,GLsizei instances){dispatch_draw(&(struct draw_call){.mode=m,.count=n,.instances=instances,.type=type,.indices=indices,.kind=4});}
void glDrawElementsInstancedBaseVertexBaseInstance(GLenum m,GLsizei n,GLenum type,const void *indices,GLsizei instances,GLint vertex,GLuint base){dispatch_draw(&(struct draw_call){.mode=m,.count=n,.instances=instances,.type=type,.indices=indices,.base_vertex=vertex,.base=base,.kind=5});}
// Nonzero base-vertex draws stay on Mesa until the device ABI carries both
// the index fetch base and shader-visible gl_BaseVertex without conflation.
void glDrawElementsBaseVertex(GLenum m,GLsizei n,GLenum type,const void *indices,GLint base){dispatch_draw(&(struct draw_call){.mode=m,.count=n,.instances=1,.type=type,.indices=indices,.base_vertex=base,.kind=6});}
void glDrawElementsInstancedBaseVertex(GLenum m,GLsizei n,GLenum type,const void *indices,GLsizei instances,GLint base){dispatch_draw(&(struct draw_call){.mode=m,.count=n,.instances=instances,.type=type,.indices=indices,.base_vertex=base,.kind=7});}
// OpenGL command layouts are fixed: 16 bytes for arrays, 20 for elements.
struct arrays_command {GLuint count,instances,first,base_instance;};
struct elements_command {GLuint count,instances,first;GLint base_vertex;GLuint base_instance;};
_Static_assert(sizeof(struct arrays_command)==16,"array indirect ABI");
_Static_assert(sizeof(struct elements_command)==20,"element indirect ABI");
static unsigned index_bytes(GLenum type){return type==GL_UNSIGNED_BYTE?1:type==GL_UNSIGNED_SHORT?2:type==GL_UNSIGNED_INT?4:0;}
static int indirect_valid(const void *p){return ((uintptr_t)p&3)==0;}
void glDrawArraysIndirect(GLenum mode,const void *indirect){struct draw_call call={.mode=mode,.indirect=indirect,.kind=8};GLint buffer=0;glGetIntegerv(GL_DRAW_INDIRECT_BUFFER_BINDING,&buffer);struct arrays_command d;
 if(!buffer||!indirect_valid(indirect)||!read_buffer_bytes(buffer,(uintptr_t)indirect,sizeof d,&d)||d.count>INT32_MAX||d.instances>INT32_MAX||d.first>INT32_MAX){mesa_draw(&call);return;}call.first=d.first;call.count=d.count;call.instances=d.instances;call.base=d.base_instance;dispatch_draw(&call);}
void glDrawElementsIndirect(GLenum mode,GLenum type,const void *indirect){struct draw_call call={.mode=mode,.type=type,.indirect=indirect,.kind=9};GLint buffer=0,element=0;glGetIntegerv(GL_DRAW_INDIRECT_BUFFER_BINDING,&buffer);glGetIntegerv(GL_ELEMENT_ARRAY_BUFFER_BINDING,&element);struct elements_command d;unsigned cb=index_bytes(type);
 if(!buffer||!element||!cb||!indirect_valid(indirect)||!read_buffer_bytes(buffer,(uintptr_t)indirect,sizeof d,&d)||d.count>INT32_MAX||d.instances>INT32_MAX||d.first>SIZE_MAX/cb){mesa_draw(&call);return;}call.count=d.count;call.instances=d.instances;call.indices=(void *)((uintptr_t)d.first*cb);call.base_vertex=d.base_vertex;call.base=d.base_instance;dispatch_draw(&call);}
static int multi_parameters(const void *indirect,GLsizei count,GLsizei stride,size_t packet,size_t *step){if(count<0||count>256||stride<0||(stride&3)||!indirect_valid(indirect))return 0;*step=stride?(size_t)stride:packet;return !count||*step<=(SIZE_MAX-(uintptr_t)indirect)/(size_t)count;}
void glMultiDrawArraysIndirect(GLenum mode,const void *indirect,GLsizei count,GLsizei stride){if(count<0||stride<0||(stride&3)){invalid_value();return;}typeof(&glMultiDrawArraysIndirect) f=original("glMultiDrawArraysIndirect");GLint buffer=0;glGetIntegerv(GL_DRAW_INDIRECT_BUFFER_BINDING,&buffer);size_t step;struct arrays_command commands[256];
 if(!buffer||!multi_parameters(indirect,count,stride,sizeof commands[0],&step)){{resident_around_mesa_draw(0,mode);if(f)f(mode,indirect,count,stride);resident_around_mesa_draw(1,0);}return;}
 // Validate every command before executing any draw; preserve the caller's stride on fallback.
 for(GLsizei i=0;i<count;i++)if(!read_buffer_bytes(buffer,(uintptr_t)indirect+(size_t)i*step,sizeof commands[i],&commands[i])||commands[i].count>INT32_MAX||commands[i].instances>INT32_MAX||commands[i].first>INT32_MAX){{resident_around_mesa_draw(0,mode);if(f)f(mode,indirect,count,stride);resident_around_mesa_draw(1,0);}return;}
 for(GLsizei i=0;i<count;i++){struct arrays_command d=commands[i];dispatch_draw(&(struct draw_call){.mode=mode,.first=d.first,.count=d.count,.instances=d.instances,.base=d.base_instance,.indirect=(void *)((uintptr_t)indirect+(size_t)i*step),.kind=8});}}
void glMultiDrawElementsIndirect(GLenum mode,GLenum type,const void *indirect,GLsizei count,GLsizei stride){if(count<0||stride<0||(stride&3)){invalid_value();return;}typeof(&glMultiDrawElementsIndirect) f=original("glMultiDrawElementsIndirect");GLint buffer=0,element=0;glGetIntegerv(GL_DRAW_INDIRECT_BUFFER_BINDING,&buffer);glGetIntegerv(GL_ELEMENT_ARRAY_BUFFER_BINDING,&element);size_t step;unsigned cb=index_bytes(type);struct elements_command commands[256];
 if(!buffer||!element||!cb||!multi_parameters(indirect,count,stride,sizeof commands[0],&step)){{resident_around_mesa_draw(0,mode);if(f)f(mode,type,indirect,count,stride);resident_around_mesa_draw(1,0);}return;}
 for(GLsizei i=0;i<count;i++)if(!read_buffer_bytes(buffer,(uintptr_t)indirect+(size_t)i*step,sizeof commands[i],&commands[i])||commands[i].count>INT32_MAX||commands[i].instances>INT32_MAX||commands[i].first>SIZE_MAX/cb){{resident_around_mesa_draw(0,mode);if(f)f(mode,type,indirect,count,stride);resident_around_mesa_draw(1,0);}return;}
 for(GLsizei i=0;i<count;i++){struct elements_command d=commands[i];dispatch_draw(&(struct draw_call){.mode=mode,.count=d.count,.instances=d.instances,.type=type,.indices=(void *)((uintptr_t)d.first*cb),.base_vertex=d.base_vertex,.base=d.base_instance,.indirect=(void *)((uintptr_t)indirect+(size_t)i*step),.kind=9});}}
GLenum glGetError(void){typeof(&glGetError) fn=original("glGetError");GLenum error=fn?fn():GL_NO_ERROR;struct bridge_context *c=current_context();if(c&&c->error){if(error==c->error)c->error=GL_NO_ERROR;else if(!error){error=c->error;c->error=GL_NO_ERROR;}}if(error)note("GL_ERROR observed_by_application=%x",error);return error;}
struct entry {const char *name;void *wrapper;_Atomic(void *) resolved;};
static void *interposed(const char *,void *);
__GLXextFuncPtr glXGetProcAddress(const GLubyte *name){typeof(&glXGetProcAddress) fn=original("glXGetProcAddress");return (__GLXextFuncPtr)interposed((const char *)name,fn?(void *)fn(name):NULL);}
__GLXextFuncPtr glXGetProcAddressARB(const GLubyte *name){typeof(&glXGetProcAddressARB) fn=original("glXGetProcAddressARB");return (__GLXextFuncPtr)interposed((const char *)name,fn?(void *)fn(name):NULL);}
__eglMustCastToProperFunctionPointerType eglGetProcAddress(const char *name){typeof(&eglGetProcAddress) fn=original("eglGetProcAddress");return (__eglMustCastToProperFunctionPointerType)interposed(name,fn?(void *)fn(name):NULL);}
#define ENTRY(fn) {#fn,(void *)fn,NULL}
static struct entry entries[]={ENTRY(glActiveTexture),ENTRY(glActiveTextureARB),ENTRY(glXCreateContextAttribsARB),ENTRY(glXCreateContext),ENTRY(glXCreateNewContext),ENTRY(glXMakeCurrent),ENTRY(glXMakeContextCurrent),ENTRY(glXDestroyContext),ENTRY(glLinkProgram),ENTRY(glGetError),ENTRY(glDrawArrays),ENTRY(glDrawRangeElements),ENTRY(glDrawArraysInstanced),ENTRY(glDrawArraysInstancedBaseInstance),ENTRY(glDrawElements),ENTRY(glDrawElementsInstanced),ENTRY(glDrawElementsInstancedBaseVertexBaseInstance),ENTRY(glDrawElementsBaseVertex),ENTRY(glDrawElementsInstancedBaseVertex),ENTRY(glDrawArraysIndirect),ENTRY(glDrawElementsIndirect),ENTRY(glMultiDrawArraysIndirect),ENTRY(glMultiDrawElementsIndirect),ENTRY(glXGetProcAddress),ENTRY(glXGetProcAddressARB),ENTRY(eglCreateContext),ENTRY(eglMakeCurrent),ENTRY(eglDestroyContext),ENTRY(eglGetProcAddress),ENTRY(eglSwapBuffers),ENTRY(eglSwapBuffersWithDamageKHR),ENTRY(eglSwapBuffersWithDamageEXT),RESIDENT_ENTRIES};
#undef ENTRY
// RTLD_NEXT resolution is fixed for the process lifetime, so cache it: hooks
// call original() on every GL call and dlsym dominated the interposer's cost.
// An entry's provider (set when the application resolves it) still wins once
// known, exactly as before. Keys are call-site literals (pointer identity).
#define ORIGINAL_CACHE 256
static struct {_Atomic(const char *) name;_Atomic(void *) fallback;_Atomic int entry;} original_cache[ORIGINAL_CACHE];
static void *original(const char *name){
 uintptr_t h=((uintptr_t)name>>3)*0x9e3779b97f4a7c15ull;
 for(unsigned probe=0;probe<16;probe++){unsigned i=(unsigned)((h>>56)+probe)%ORIGINAL_CACHE;const char *key=atomic_load(&original_cache[i].name);
  if(key==name){void *v=atomic_load(&original_cache[i].fallback);if(!v)break;int e=atomic_load(&original_cache[i].entry);
   if(e>=0){void *p=atomic_load(&entries[e].resolved);if(p)return p;}return v;}
  if(!key)break;}
 int entry=-1;void *found=NULL;for(unsigned i=0;i<sizeof entries/sizeof *entries;i++)if(!strcmp(name,entries[i].name)){entry=(int)i;found=atomic_load(&entries[i].resolved);break;}
 void *fallback=lookup(RTLD_NEXT,name);if(!found)found=fallback;if(!fallback)return found;
 for(unsigned probe=0;probe<16;probe++){unsigned i=(unsigned)((h>>56)+probe)%ORIGINAL_CACHE;const char *expected=NULL;
  if(atomic_compare_exchange_strong(&original_cache[i].name,&expected,name)||expected==name){atomic_store(&original_cache[i].entry,entry);atomic_store(&original_cache[i].fallback,fallback);break;}}
 return found;
}
static void *interposed(const char *name,void *pointer){if(!pointer||!name||name[0]!='g'||name[1]!='l')return pointer;   // only gl*/glX* names are interposed; everything else (any process may preload this) passes straight through
 for(unsigned i=0;i<sizeof entries/sizeof *entries;i++)if(!strcmp(name,entries[i].name)){struct entry *e=&entries[i];if(pointer==e->wrapper)return pointer;void *expected=NULL;if(!atomic_compare_exchange_strong(&e->resolved,&expected,pointer)&&expected!=pointer){note("provider_change function=%s keeping_original=1",name);{static const char *const epoch_prefixes[]={"glTex","glTexture","glSampler","glDeleteSamplers","glCompressedTex","glCopyTex","glGenerate","glDeleteTextures","glPopAttrib","glCallList"};for(unsigned k=0;k<sizeof epoch_prefixes/sizeof *epoch_prefixes;k++)if(!strncmp(name,epoch_prefixes[k],strlen(epoch_prefixes[k]))){atomic_store(&texture_epoch_unsafe,1);note("texture_cache_disabled function=%s",name);}}if(!strncmp(name,"glActiveTexture",15))atomic_store(&ff_units_seen,32);   /* unit selections may bypass the hook: scan every unit */
 return pointer;}return e->wrapper;}return pointer;}
void *dlsym(void *handle,const char *name){return interposed(name,lookup(handle,name));}
// ---- device-visible statistics ------------------------------------------------
// LR_MESA_METAL_STATS_DIR=<dir>: each GL process keeps <dir>/<pid>.txt current
// (at most once a second, at swap, and at context teardown) and logs a GPU_STATS
// line every 10 s. It separates guest draws executed by Metal from draws Mesa
// rasterised, which final Metal presentation of the desktop says nothing about.
static void stats_write(int force){
 const char *dir=config("LR_MESA_METAL_STATS_DIR");if(!dir||!*dir)return;
 atomic_store(&report_enabled,1);
 static pthread_mutex_t lock=PTHREAD_MUTEX_INITIALIZER;static uint64_t first,last_write,last_log,last_frames;static char comm[32];
 if(force?pthread_mutex_lock(&lock):pthread_mutex_trylock(&lock))return;
 struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);uint64_t now=(uint64_t)t.tv_sec*1000000000ull+(uint64_t)t.tv_nsec;
 if(!first){first=now;FILE *f=fopen("/proc/self/comm","r");if(f){if(fgets(comm,sizeof comm,f))comm[strcspn(comm,"\n")]=0;fclose(f);}}
 if(!force&&now-last_write<1000000000ull){pthread_mutex_unlock(&lock);return;}
 uint64_t metal=0,mesa=0,verified=0,mismatched=0,gpu_ns=0,submissions=0,clears=0,uploads=0,writebacks=0;unsigned live=0;
 pthread_mutex_lock(&records);
 for(unsigned i=0;i<CONTEXTS;i++){struct bridge_context *c=&contexts[i];if(!c->handle)continue;live++;
  if(c->fd>=0&&c->owner==current_tid()){struct lr_gpu_request r={.operation=LR_GPU_INFO};if(call_gpu(c,&r)){c->stats_gpu_ns=r.gpu_nanoseconds;c->stats_submissions=r.submissions;}}
  metal+=c->committed;mesa+=c->fallback;verified+=c->verified;mismatched+=c->mismatched;gpu_ns+=c->stats_gpu_ns;submissions+=c->stats_submissions;
  if(c->resident){clears+=c->resident->gpu_clears;uploads+=c->resident->uploads;writebacks+=c->resident->writebacks;}}
 pthread_mutex_unlock(&records);
 uint64_t frames=atomic_load(&stats_frames);double seconds=last_write?(now-last_write)/1e9:(now-first)/1e9;double fps=seconds>0?(frames-last_frames)/seconds:0;
 char reasons[512]={0};size_t used=0;pthread_mutex_lock(&report_lock);
 uint64_t upload_bytes=report_bytes[REPORT_UPLOAD],writeback_bytes=report_bytes[REPORT_WRITEBACK];
 unsigned char taken[64]={0};   // the six most frequent fallback reasons
 for(unsigned n=0;n<6;n++){int best=-1;for(unsigned i=0;i<64&&report_reasons[i].reason;i++)if(!taken[i]&&(best<0||report_reasons[i].count>report_reasons[best].count))best=(int)i;
  if(best<0||used>=sizeof reasons-1)break;taken[best]=1;used+=snprintf(reasons+used,sizeof reasons-used,"%s%s:%llu",used?",":"",report_reasons[best].reason,(unsigned long long)report_reasons[best].count);}
 pthread_mutex_unlock(&report_lock);
 // LR_MESA_METAL_PHASES=1: per-draw phase timers (cumulative count/ms per phase).
 static int phases=-1;if(phases<0){phases=config("LR_MESA_METAL_PHASES")&&!strcmp(config("LR_MESA_METAL_PHASES"),"1");if(phases)atomic_store(&report_phases,1);}
 char timing[800]={0};
 if(phases){static const char *names[REPORT_SLOTS]={"upload","writeback","writeback_window","wb_read","gpu_draw","mesa_draw","p_state","p_targets","p_program","p_uniforms","p_vertices","p_state_ioctl","p_draw_ioctl","p_ff","p_lookup","p_indices","p_u_samplers","p_u_blocks"};size_t at=0;
  pthread_mutex_lock(&report_lock);for(unsigned i=0;i<REPORT_SLOTS&&at<sizeof timing;i++)at+=snprintf(timing+at,sizeof timing-at,"%s%s=%llu/%.0fms",i?",":"",names[i],(unsigned long long)report_count[i],report_ns[i]/1e6);pthread_mutex_unlock(&report_lock);}
 const char *mode=config("LR_MESA_METAL_MODE");int resident=resident_enabled();uint64_t retired_at=atomic_load(&bridge_retired_ns);
 char text[2048];int len=snprintf(text,sizeof text,
  "pid=%ld\ncommand=%s\nmode=%s\nresident=%d\ncontexts=%u\nuptime_ms=%llu\nupdated_unix=%lld\nframes=%llu\nfps=%.1f\n"
  "metal_draws=%llu\nmesa_draws=%llu\nverified=%llu\nmismatched=%llu\nmetal_clears=%llu\nmetal_blits=%llu\n"
  "uploads=%llu\nupload_bytes=%llu\nwritebacks=%llu\nwriteback_bytes=%llu\n"
  "program_compiles=%llu\nprogram_cache_hits=%llu\nprogram_failures=%llu\ngpu_ms=%.1f\nmetal_submissions=%llu\nreasons=%s\nphases=%s\n"
  "retired_frame=%llu\nfps_after_retire=%.2f\n",
  (long)getpid(),comm,mode?mode:"off",resident,live,(unsigned long long)((now-first)/1000000),(long long)time(NULL),(unsigned long long)frames,fps,
  (unsigned long long)metal,(unsigned long long)mesa,(unsigned long long)verified,(unsigned long long)mismatched,(unsigned long long)clears,(unsigned long long)atomic_load(&stats_blits),
  (unsigned long long)uploads,(unsigned long long)upload_bytes,(unsigned long long)writebacks,(unsigned long long)writeback_bytes,
  (unsigned long long)atomic_load(&stats_program_compiles),(unsigned long long)atomic_load(&stats_program_hits),(unsigned long long)atomic_load(&stats_program_failures),gpu_ns/1e6,(unsigned long long)submissions,reasons[0]?reasons:"none",timing[0]?timing:"off",
  (unsigned long long)atomic_load(&bridge_retired_frames),retired_at&&now>retired_at?(double)(frames-atomic_load(&bridge_retired_frames))*1e9/(double)(now-retired_at):0.0);
 if(len>0&&(size_t)len<sizeof text){char path[1024],tmp[1040];if(snprintf(path,sizeof path,"%s/%ld.txt",dir,(long)getpid())<(int)sizeof path){snprintf(tmp,sizeof tmp,"%s.tmp",path);
  FILE *f=fopen(tmp,"w");if(f){fwrite(text,1,(size_t)len,f);if(fclose(f)==0)rename(tmp,path);else remove(tmp);}}}
 if(force||now-last_log>=10000000000ull){last_log=now;fprintf(stderr,"GPU_STATS pid=%ld command=%s mode=%s resident=%d frames=%llu fps=%.1f metal_draws=%llu mesa_draws=%llu uploads=%llu/%lluKB writebacks=%llu/%lluKB programs=%llu+%llu_hits gpu_ms=%.1f reasons=%s\n",
  (long)getpid(),comm,mode?mode:"off",resident,(unsigned long long)frames,fps,(unsigned long long)metal,(unsigned long long)mesa,(unsigned long long)uploads,(unsigned long long)(upload_bytes>>10),(unsigned long long)writebacks,(unsigned long long)(writeback_bytes>>10),
  (unsigned long long)atomic_load(&stats_program_compiles),(unsigned long long)atomic_load(&stats_program_hits),gpu_ns/1e6,reasons[0]?reasons:"none");}
 last_write=now;last_frames=frames;pthread_mutex_unlock(&lock);
}
int lrMesaMetalStatistics(uint64_t *committed,uint64_t *verified,uint64_t *mismatched,uint64_t *fallback,uint64_t *submissions,uint64_t *nanoseconds){struct bridge_context *c=current_context();if(!c||c->fd<0)return 0;struct lr_gpu_request r={.operation=LR_GPU_INFO};if(!call_gpu(c,&r))return 0;if(committed)*committed=c->committed;if(verified)*verified=c->verified;if(mismatched)*mismatched=c->mismatched;if(fallback)*fallback=c->fallback;if(submissions)*submissions=r.submissions;if(nanoseconds)*nanoseconds=r.gpu_nanoseconds;return 1;}
