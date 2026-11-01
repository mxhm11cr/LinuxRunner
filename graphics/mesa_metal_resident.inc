// GPU-resident render targets and textures for the Mesa bridge.
// Included by mesa_metal_dispatch.c. Enabled by LR_MESA_METAL_RESIDENT=1 in
// replace mode.
//
// Mesa keeps every GL object and remains the semantic owner. For each GL
// texture the bridge renders to or samples on the GPU it keeps a Metal copy
// (LR_GPU_RESIDENT). A share-group-wide generation per texture says which copy
// is current:
//   gen       bumped by every modification (Metal draw/clear, Mesa write)
//   mesa_gen  generation Mesa's storage holds
//   record->metal_gen  generation this context's Metal copy holds
// Metal draws use Metal copies at the current generation (uploading from Mesa
// only when Metal's copy is stale) and leave Mesa behind. Mesa is brought up to
// date (glTexSubImage, or a blit for the window) before anything that can
// observe or modify Mesa's storage: a Mesa draw, clears that cannot run on the
// GPU, reads, blits, copies, texture uploads, mipmap generation, flush/finish,
// fences, context switches and buffer swaps. GL requires a flush before another
// context sees a change, so syncing at those points preserves cross-context
// visibility.
#define RESIDENT_RECORDS 512
#define RESIDENT_TEXTURES 4096
enum resident_part {RESIDENT_COLOR, RESIDENT_DEPTH, RESIDENT_STENCIL};
struct resident {
 GLuint name;            // GL texture name; 0 = this context's window back buffer
 enum resident_part part;
 unsigned id,width,height,format,channels;
 GLenum internal,type;   // GL internal format; client type for transfers
 uint64_t metal_gen;
 int in_use;
};
struct texture_query {GLuint texture,sampler;uint64_t epoch;GLint internal,w,h,base,min,mag,ws,wt,compare,mode,decode,swizzle[4];};
struct resident_state {
 struct texture_query texture_queries[32];
 uint16_t find_hint[1024];   // (name,part) -> record index + 1; verified on use, a miss scans
 struct resident record[RESIDENT_RECORDS];
 unsigned next_id;
 unsigned limit;         // records at index >= limit have never been used
 int bound;              // backend has resident targets bound
 GLuint bound_names[6];  // texture names behind the bound attachments
 unsigned bound_count;
 uint64_t uploads,writebacks,draws,clears,syncs,gpu_clears,mesa_draws;
};
// Share-group generations (texture names are shared within a group).
struct texture_generation {uintptr_t group;GLuint name;uint64_t gen,mesa_gen;};
static struct texture_generation generations[RESIDENT_TEXTURES];
static pthread_mutex_t generation_lock=PTHREAD_MUTEX_INITIALIZER;
static uint64_t window_generation_base=1ull<<62;

// Set when this process's mix of Mesa and Metal draws made the bridge slower
// than Mesa alone (see glXSwapBuffers): everything has been synced back to Mesa
// and the process stays on Mesa from then on.
static _Atomic int bridge_retired;
// Texture/sampler parameter epoch: every GL entry point that can change what
// resident_sampler reads bumps it, so unchanged textures skip ~18 queries per
// draw. If an application reaches one of those entry points around the hooks
// (provider_change), the cache stays off for the process.
static _Atomic uint64_t texture_epoch=1;static _Atomic int texture_epoch_unsafe;
static inline void texture_state_changed(void){atomic_fetch_add_explicit(&texture_epoch,1,memory_order_relaxed);}

static _Atomic uint64_t bridge_retired_ns,bridge_retired_frames;   // when retirement happened (stats: FPS after it)
static int resident_enabled(void){
 if(atomic_load_explicit(&bridge_retired,memory_order_relaxed))return 0;
 const char *mode=config("LR_MESA_METAL_MODE"),*resident=config("LR_MESA_METAL_RESIDENT");
 return mode&&!strcmp(mode,"replace")&&resident&&!strcmp(resident,"1")&&!es_current();   /* GPU-resident images: desktop GL contexts only */
}
// Open addressing keyed by (group, name); a slot is free when group == 0 (groups
// start at 1, window entries use a high-bit group with name 0). Generation
// values come from one process-wide clock, so an entry removed on texture
// deletion and later recreated for a recycled name can never match a stale
// Metal copy in another context. Callers hold generation_lock.
static uint64_t generation_clock;
static unsigned generation_home(uintptr_t group,GLuint name){return (unsigned)((name*2654435761u)^(unsigned)(group>>4)^(unsigned)group)%RESIDENT_TEXTURES;}
static struct texture_generation *generation_entry(uintptr_t group,GLuint name,int create){
 unsigned h=generation_home(group,name);
 for(unsigned i=0;i<RESIDENT_TEXTURES;i++){struct texture_generation *g=&generations[(h+i)%RESIDENT_TEXTURES];
  if(g->group==group&&g->name==name)return g;
  if(!g->group){if(!create)return NULL;g->group=group;g->name=name;g->gen=g->mesa_gen=++generation_clock;return g;}}
 return NULL;
}
static void generation_remove(uintptr_t group,GLuint name){
 unsigned i=generation_home(group,name),n=0;
 while(n<RESIDENT_TEXTURES&&generations[i].group&&!(generations[i].group==group&&generations[i].name==name)){i=(i+1)%RESIDENT_TEXTURES;n++;}
 if(n>=RESIDENT_TEXTURES||!generations[i].group)return;
 for(unsigned j=i;;){j=(j+1)%RESIDENT_TEXTURES;if(!generations[j].group)break;   // backward-shift deletion
  unsigned k=generation_home(generations[j].group,generations[j].name);
  if(i<=j?(i<k&&k<=j):(i<k||k<=j))continue;
  generations[i]=generations[j];i=j;}
 memset(&generations[i],0,sizeof generations[i]);
}
// The window back buffer is per context; key it by the context id. The window
// depth buffer is a separate image with its own generation (a draw that writes
// colour and depth must not make either look stale): reserved name below.
#define RESIDENT_WINDOW_DEPTH 0xFFFFFFFFu
static int resident_window_name(GLuint name){return name==0||name==RESIDENT_WINDOW_DEPTH;}
static uintptr_t resident_group(struct bridge_context *c,GLuint name){return resident_window_name(name)?(uintptr_t)c->id|window_generation_base:c->group;}


// The bridge's own queries must not leak GL errors to the application, and an
// error the application has not read yet must survive them.
static GLenum resident_errors_begin(void){typeof(&glGetError) e=original("glGetError");return e?e():GL_NO_ERROR;}
static void resident_errors_end(struct bridge_context *c,GLenum pending){
 typeof(&glGetError) e=original("glGetError");if(e)for(unsigned i=0;i<8&&e()!=GL_NO_ERROR;i++){}
 if(pending&&c&&!c->error)c->error=pending;
}
// Open the GPU device for this context (as gpu_draw does) before the first draw.
static int resident_device(struct bridge_context *c,unsigned w,unsigned h){
 if(c->fd>=0)return 1;
 c->fd=open("/dev/linuxrunner-gpu",O_RDWR|O_CLOEXEC);if(c->fd<0)return 0;
 struct lr_gpu_request r={.operation=LR_GPU_CREATE,.width=w,.height=h};
 if(!call_gpu(c,&r)){close(c->fd);c->fd=-1;return 0;}c->width=w;c->height=h;return 1;
}
static struct resident_state *resident_state(struct bridge_context *c){
 if(!c->resident){c->resident=calloc(1,sizeof *c->resident);if(c->resident)c->resident->next_id=1;}
 return c->resident;
}
static int resident_call(struct bridge_context *c,struct lr_gpu_resident *h,void *pixels,size_t bytes){
 size_t total=sizeof *h+bytes;unsigned char *packet=malloc(total);if(!packet)return 0;
 memcpy(packet,h,sizeof *h);if(pixels&&h->command==LR_GPU_RES_UPLOAD)memcpy(packet+sizeof *h,pixels,bytes);
 struct lr_gpu_request r={.operation=LR_GPU_RESIDENT,.address=(uintptr_t)packet,.bytes=total};int ok=call_gpu(c,&r);
 if(ok&&h->command==LR_GPU_RES_READ)memcpy(pixels,packet+sizeof *h,bytes);
 free(packet);return ok;
}
static struct resident *resident_find(struct resident_state *s,GLuint name,enum resident_part part){
 unsigned h=(name*4u+(unsigned)part)%1024u,k=s->find_hint[h];
 if(k&&k<=s->limit){struct resident *r=&s->record[k-1];if(r->in_use&&r->name==name&&r->part==part)return r;}
 for(unsigned i=0;i<s->limit;i++)if(s->record[i].in_use&&s->record[i].name==name&&s->record[i].part==part){s->find_hint[h]=(uint16_t)(i+1);return &s->record[i];}
 return NULL;
}
static void resident_drop(struct bridge_context *c,struct resident *r){
 struct lr_gpu_resident h={.command=LR_GPU_RES_DELETE,.id=r->id};resident_call(c,&h,NULL,0);memset(r,0,sizeof *r);
}

// GL internal format -> transfer marker/type/channels for colour storage.
static int colour_format(GLint internal,unsigned *format,GLenum *type,unsigned *channels){
 switch(internal){
  case GL_RGBA8:case GL_RGBA:*format=0x8058;*type=GL_UNSIGNED_BYTE;*channels=15;return 1;
  // Legacy and packed formats: transferred as RGBA bytes (GL's documented
  // expansion: RGB -> A=1, LUMINANCE/INTENSITY -> R, ALPHA -> A); fixed-function
  // shaders apply the base-format rules themselves.
  case GL_RGBA4:case GL_RGB5_A1:case 4:*format=0x8058;*type=GL_UNSIGNED_BYTE;*channels=15;return 1;
  case GL_RGB8:case GL_RGB:case GL_RGB5:case GL_RGB565:case 3:*format=0x8058;*type=GL_UNSIGNED_BYTE;*channels=7;return 1;
  case GL_LUMINANCE8:case GL_LUMINANCE:case 1:case GL_ALPHA8:case GL_ALPHA:case GL_LUMINANCE8_ALPHA8:case GL_LUMINANCE_ALPHA:case 2:case GL_INTENSITY8:case GL_INTENSITY:
   *format=0x8058;*type=GL_UNSIGNED_BYTE;*channels=15;return 1;
  case GL_RG8:*format=LR_GPU_RES_FORMAT_RG8;*type=GL_UNSIGNED_BYTE;*channels=3;return 1;
  case GL_R8:*format=LR_GPU_RES_FORMAT_R8;*type=GL_UNSIGNED_BYTE;*channels=1;return 1;
  case GL_SRGB8_ALPHA8:*format=LR_GPU_RES_FORMAT_SRGB8_A8;*type=GL_UNSIGNED_BYTE;*channels=15;return 1;   // raw bytes both ways
  case GL_RGBA16F:*format=0x881a;*type=GL_HALF_FLOAT;*channels=15;return 1;
  case GL_RG16F:*format=0x881a;*type=GL_HALF_FLOAT;*channels=3;return 1;
  case GL_R16F:*format=0x881a;*type=GL_HALF_FLOAT;*channels=1;return 1;
  case GL_RGBA16UI:*format=0x8d76;*type=GL_UNSIGNED_SHORT;*channels=15;return 1;
  case GL_RG16UI:*format=0x8d76;*type=GL_UNSIGNED_SHORT;*channels=3;return 1;
  case GL_R16UI:*format=0x8d76;*type=GL_UNSIGNED_SHORT;*channels=1;return 1;
  case GL_RGBA16I:*format=0x8d88;*type=GL_SHORT;*channels=15;return 1;
  case GL_RGBA32UI:*format=0x8d70;*type=GL_UNSIGNED_INT;*channels=15;return 1;
  case GL_R32UI:*format=0x8d70;*type=GL_UNSIGNED_INT;*channels=1;return 1;
  case GL_RGBA32I:*format=0x8d82;*type=GL_INT;*channels=15;return 1;
  case GL_R32I:*format=0x8d82;*type=GL_INT;*channels=1;return 1;
  default:return 0;
 }
}
static int integer_type(GLenum type){return type==GL_UNSIGNED_SHORT||type==GL_SHORT||type==GL_UNSIGNED_INT||type==GL_INT;}
static int depth_internal(GLint internal){return internal==GL_DEPTH_COMPONENT32F||internal==GL_DEPTH_COMPONENT32||internal==GL_DEPTH_COMPONENT24||internal==GL_DEPTH_COMPONENT16||internal==GL_DEPTH_COMPONENT||internal==GL_DEPTH24_STENCIL8||internal==GL_DEPTH32F_STENCIL8;}
static int packed_depth_stencil(GLint internal){return internal==GL_DEPTH24_STENCIL8||internal==GL_DEPTH32F_STENCIL8;}

// Level-0 description of a 2D texture; restores the caller's binding.
static int texture_level(GLuint name,GLint *internal,GLint *w,GLint *h){
 GLint prior;glGetIntegerv(GL_TEXTURE_BINDING_2D,&prior);glBindTexture(GL_TEXTURE_2D,name);
 glGetTexLevelParameteriv(GL_TEXTURE_2D,0,GL_TEXTURE_INTERNAL_FORMAT,internal);glGetTexLevelParameteriv(GL_TEXTURE_2D,0,GL_TEXTURE_WIDTH,w);glGetTexLevelParameteriv(GL_TEXTURE_2D,0,GL_TEXTURE_HEIGHT,h);
 glBindTexture(GL_TEXTURE_2D,prior);return *w>0&&*h>0;   // callers drain bridge-caused errors
}

// Find or (re)define a record mirroring a texture part (or the window).
static struct resident *resident_record(struct bridge_context *c,GLuint name,enum resident_part part,unsigned w,unsigned h,unsigned format,GLenum internal,GLenum type,unsigned channels){
 struct resident_state *s=resident_state(c);if(!s||!resident_device(c,w,h))return NULL;
 struct resident *r=resident_find(s,name,part);
 if(r&&(r->width!=w||r->height!=h||r->format!=format)){resident_drop(c,r);r=NULL;}
 if(!r){for(unsigned i=0;i<RESIDENT_RECORDS&&!r;i++)if(!s->record[i].in_use)r=&s->record[i];
  if(!r){note("resident_fallback reason=records");return NULL;}
  unsigned id=0;for(unsigned tries=0;tries<LR_GPU_MAX_RESIDENT&&!id;tries++){unsigned candidate=s->next_id;s->next_id=s->next_id%LR_GPU_MAX_RESIDENT+1;int taken=0;for(unsigned i=0;i<s->limit;i++)if(s->record[i].in_use&&s->record[i].id==candidate)taken=1;if(!taken)id=candidate;}
  if(!id)return NULL;
  struct lr_gpu_resident d={.command=LR_GPU_RES_DEFINE,.id=id,.width=w,.height=h,.format=format};
  if(!resident_call(c,&d,NULL,0)){note("resident_fallback reason=define format=%x size=%ux%u",format,w,h);return NULL;}
  *r=(struct resident){.name=name,.part=part,.id=id,.width=w,.height=h,.format=format,.channels=channels,.internal=internal,.type=type,.in_use=1};
  if((unsigned)(r-s->record)>=s->limit)s->limit=(unsigned)(r-s->record)+1;
 }
 return r;
}

// ---- transfers -----------------------------------------------------------
// Client format for whole-image transfers; matches the Metal storage layout.
static GLenum transfer_format(const struct resident *r){
 if(integer_type(r->type))return GL_RGBA_INTEGER;
 return r->format==LR_GPU_RES_FORMAT_R8?GL_RED:r->format==LR_GPU_RES_FORMAT_RG8?GL_RG:GL_RGBA;
}
static size_t resident_bytes(const struct resident *r){return (size_t)r->width*r->height*lr_gpu_resident_pixel_bytes(r->format);}

// Mesa -> Metal for texture parts (window colour uses read_color).
static int resident_upload(struct bridge_context *c,struct resident *r,uint64_t gen){
 size_t bytes=resident_bytes(r);void *pixels=malloc(bytes?bytes:1);if(!pixels)return 0;uint64_t started=report_now();
 struct pixel_state pack;pixel_begin(&pack,1);
 if(r->name==RESIDENT_WINDOW_DEPTH){   // window depth buffer
  GLint read_fb;glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING,&read_fb);glBindFramebuffer(GL_READ_FRAMEBUFFER,0);
  glReadPixels(0,0,r->width,r->height,GL_DEPTH_COMPONENT,GL_FLOAT,pixels);glBindFramebuffer(GL_READ_FRAMEBUFFER,read_fb);}
 else if(!r->name){   // window back buffer, independent of the current bindings
  GLint read_fb;glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING,&read_fb);glBindFramebuffer(GL_READ_FRAMEBUFFER,0);GLint window_read=GL_BACK;glGetIntegerv(GL_READ_BUFFER,&window_read);
  glReadBuffer(GL_BACK);glReadPixels(0,0,r->width,r->height,GL_RGBA,GL_UNSIGNED_BYTE,pixels);glReadBuffer(window_read);glBindFramebuffer(GL_READ_FRAMEBUFFER,read_fb);}
 else{GLint prior;glGetIntegerv(GL_TEXTURE_BINDING_2D,&prior);glBindTexture(GL_TEXTURE_2D,r->name);
  if(r->part==RESIDENT_COLOR)glGetTexImage(GL_TEXTURE_2D,0,transfer_format(r),r->type,pixels);
  else if(!packed_depth_stencil(r->internal)){if(r->part==RESIDENT_DEPTH)glGetTexImage(GL_TEXTURE_2D,0,GL_DEPTH_COMPONENT,GL_FLOAT,pixels);}
  else{uint32_t *packed=malloc((size_t)r->width*r->height*4);if(!packed){glBindTexture(GL_TEXTURE_2D,prior);pixel_end(&pack,1);free(pixels);return 0;}
   glGetTexImage(GL_TEXTURE_2D,0,GL_DEPTH_STENCIL,GL_UNSIGNED_INT_24_8,packed);
   for(size_t i=0;i<(size_t)r->width*r->height;i++){if(r->part==RESIDENT_DEPTH)((float *)pixels)[i]=(float)(packed[i]>>8)/16777215.0f;else ((unsigned char *)pixels)[i]=packed[i]&255;}
   free(packed);}
  glBindTexture(GL_TEXTURE_2D,prior);}
 pixel_end(&pack,1);
 struct lr_gpu_resident h={.command=LR_GPU_RES_UPLOAD,.id=r->id};int ok=resident_call(c,&h,pixels,bytes);free(pixels);
 if(ok){r->metal_gen=gen;resident_state(c)->uploads++;report_add(REPORT_UPLOAD,started,bytes);}
 return ok;
}

// float depth -> 24-bit unorm, round half to even like lrintf, without a libm
// call per pixel (that call dominated packed depth writebacks).
static inline uint32_t depth_unorm24(float d){
 d=!(d>0)?0:d>1?1:d;   // NaN -> 0, as fmaxf(NaN,0)
 float v=d*16777215.0f;uint32_t r=(uint32_t)v;float f=v-(float)r;
 return r+(f>0.5f||(f==0.5f&&(r&1)));
}
// Metal -> Mesa for the window depth buffer: a depth blit from a transfer
// texture (works in core and compatibility contexts when the window depth
// format is the usual 24-bit depth with 8-bit stencil), else glDrawPixels
// (compatibility contexts). All state either path touches is restored.
static GLuint window_depth_texture,window_depth_fbo;static unsigned window_depth_w,window_depth_h;
static int resident_window_depth_store(struct bridge_context *c,struct resident *r,const float *depth){
 (void)c;size_t n=(size_t)r->width*r->height;uint32_t *packed=malloc(n*4);if(!packed)return 0;
 for(size_t i=0;i<n;i++)packed[i]=depth_unorm24(depth[i])<<8;
 typeof(&glGetError) error=original("glGetError");if(error)while(error()!=GL_NO_ERROR){}
 GLint read_fb,draw_fb,texture;glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING,&read_fb);glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING,&draw_fb);glGetIntegerv(GL_TEXTURE_BINDING_2D,&texture);
 GLboolean scissor=glIsEnabled(GL_SCISSOR_TEST);if(scissor)glDisable(GL_SCISSOR_TEST);
 typeof(&glTexImage2D) image=original("glTexImage2D");
 if(!window_depth_texture)glGenTextures(1,&window_depth_texture);if(!window_depth_fbo)glGenFramebuffers(1,&window_depth_fbo);
 glBindTexture(GL_TEXTURE_2D,window_depth_texture);
 if(window_depth_w!=r->width||window_depth_h!=r->height){image(GL_TEXTURE_2D,0,GL_DEPTH24_STENCIL8,r->width,r->height,0,GL_DEPTH_STENCIL,GL_UNSIGNED_INT_24_8,packed);window_depth_w=r->width;window_depth_h=r->height;
  glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,GL_NEAREST);glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,GL_NEAREST);}
 else{typeof(&glTexSubImage2D) sub=original("glTexSubImage2D");sub(GL_TEXTURE_2D,0,0,0,r->width,r->height,GL_DEPTH_STENCIL,GL_UNSIGNED_INT_24_8,packed);}
 glBindFramebuffer(GL_READ_FRAMEBUFFER,window_depth_fbo);glFramebufferTexture2D(GL_READ_FRAMEBUFFER,GL_DEPTH_STENCIL_ATTACHMENT,GL_TEXTURE_2D,window_depth_texture,0);
 glBindFramebuffer(GL_DRAW_FRAMEBUFFER,0);
 typeof(&glBlitFramebuffer) blit=original("glBlitFramebuffer");if(blit)blit(0,0,r->width,r->height,0,0,r->width,r->height,GL_DEPTH_BUFFER_BIT,GL_NEAREST);
 int ok=error?error()==GL_NO_ERROR:1;
 glBindFramebuffer(GL_READ_FRAMEBUFFER,read_fb);glBindFramebuffer(GL_DRAW_FRAMEBUFFER,draw_fb);glBindTexture(GL_TEXTURE_2D,texture);
 if(!ok){   // formats differ: compatibility-context fallback
  GLint program=0,depth_func=GL_LESS;GLboolean depth_test=glIsEnabled(GL_DEPTH_TEST),depth_mask=GL_TRUE,mask[4],alpha=glIsEnabled(GL_ALPHA_TEST),stencil=glIsEnabled(GL_STENCIL_TEST);
  glGetIntegerv(GL_CURRENT_PROGRAM,&program);glGetIntegerv(GL_DEPTH_FUNC,&depth_func);glGetBooleanv(GL_DEPTH_WRITEMASK,&depth_mask);glGetBooleanv(GL_COLOR_WRITEMASK,mask);
  GLfloat raster[4];glGetFloatv(GL_CURRENT_RASTER_POSITION,raster);GLint zoom_x=1;GLfloat zx=1,zy=1;glGetFloatv(GL_ZOOM_X,&zx);glGetFloatv(GL_ZOOM_Y,&zy);(void)zoom_x;
  glBindFramebuffer(GL_DRAW_FRAMEBUFFER,0);glUseProgram(0);glEnable(GL_DEPTH_TEST);glDepthFunc(GL_ALWAYS);glDepthMask(GL_TRUE);glColorMask(0,0,0,0);if(alpha)glDisable(GL_ALPHA_TEST);if(stencil)glDisable(GL_STENCIL_TEST);glPixelZoom(1,1);
  glWindowPos2i(0,0);glDrawPixels(r->width,r->height,GL_DEPTH_COMPONENT,GL_FLOAT,depth);
  ok=error?error()==GL_NO_ERROR:1;
  glWindowPos3f(raster[0],raster[1],raster[2]);glPixelZoom(zx,zy);if(stencil)glEnable(GL_STENCIL_TEST);if(alpha)glEnable(GL_ALPHA_TEST);glColorMask(mask[0],mask[1],mask[2],mask[3]);glDepthMask(depth_mask);glDepthFunc(depth_func);if(!depth_test)glDisable(GL_DEPTH_TEST);glUseProgram(program);glBindFramebuffer(GL_DRAW_FRAMEBUFFER,draw_fb);
  if(!ok)note("resident_window_depth_writeback_failed");
 }
 if(scissor)glEnable(GL_SCISSOR_TEST);free(packed);if(error)while(error()!=GL_NO_ERROR){}
 return ok;
}
// Metal -> Mesa. Depth/stencil parts of one texture are written together.
static int resident_writeback(struct bridge_context *c,struct resident *r){
 struct resident_state *s=resident_state(c);size_t bytes=resident_bytes(r);void *pixels=malloc(bytes?bytes:1);if(!pixels)return 0;uint64_t started=report_now();
 struct lr_gpu_resident h={.command=LR_GPU_RES_READ,.id=r->id};if(!resident_call(c,&h,pixels,bytes)){free(pixels);return 0;}
 report_add(REPORT_WB_READ,started,bytes);
 struct pixel_state unpack;pixel_begin(&unpack,0);
 if(r->name==RESIDENT_WINDOW_DEPTH){
  int ok=resident_window_depth_store(c,r,(const float *)pixels);pixel_end(&unpack,0);free(pixels);
  if(!ok)return 0;s->writebacks++;report_add(REPORT_WRITEBACK_WINDOW,started,bytes);return 1;
 }
 if(!r->name){
  // Window: write through the bridge's existing colour transfer.
  if(c->pixel_capacity<bytes){unsigned char *grown=realloc(c->after,bytes);if(!grown){pixel_end(&unpack,0);free(pixels);return 0;}c->after=grown;c->pixel_capacity=bytes;
   unsigned char *b=realloc(c->before,bytes),*o=realloc(c->oracle,bytes);if(b)c->before=b;if(o)c->oracle=o;}
  memcpy(c->after,pixels,bytes);unsigned saved_w=c->width,saved_h=c->height,saved_count=c->target_count;c->width=r->width;c->height=r->height;c->target_count=1;
  struct color_target t={.buffer=GL_BACK,.format=GL_RGBA8,.type=GL_UNSIGNED_BYTE,.channels=r->channels,.bytes=4,.offset=0,.size=bytes};
  // The window's own draw-buffer state is saved and restored with framebuffer 0
  // bound, whatever the application has bound now.
  GLint draw_fb;glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING,&draw_fb);glBindFramebuffer(GL_DRAW_FRAMEBUFFER,0);
  GLint window_buffer=GL_BACK;glGetIntegerv(GL_DRAW_BUFFER,&window_buffer);GLboolean discard=glIsEnabled(GL_RASTERIZER_DISCARD);if(discard)glDisable(GL_RASTERIZER_DISCARD);
  struct color_target saved_target=c->targets[0];c->targets[0]=t;present_color(c,t);c->targets[0]=saved_target;glDrawBuffer(window_buffer);if(discard)glEnable(GL_RASTERIZER_DISCARD);glBindFramebuffer(GL_DRAW_FRAMEBUFFER,draw_fb);
  c->width=saved_w;c->height=saved_h;c->target_count=saved_count;
 }else{
  GLint prior;glGetIntegerv(GL_TEXTURE_BINDING_2D,&prior);glBindTexture(GL_TEXTURE_2D,r->name);
  typeof(&glTexSubImage2D) sub=original("glTexSubImage2D");
  if(r->part==RESIDENT_COLOR)sub(GL_TEXTURE_2D,0,0,0,r->width,r->height,transfer_format(r),r->type,pixels);
  else if(!packed_depth_stencil(r->internal)){if(r->part==RESIDENT_DEPTH)sub(GL_TEXTURE_2D,0,0,0,r->width,r->height,GL_DEPTH_COMPONENT,GL_FLOAT,pixels);}
  else{
   // Combine with the other part: from Metal if it is current, else from Mesa.
   struct resident *other=resident_find(s,r->name,r->part==RESIDENT_DEPTH?RESIDENT_STENCIL:RESIDENT_DEPTH);
   size_t n=(size_t)r->width*r->height;uint32_t *packed=malloc(n*4);
   if(packed){glGetTexImage(GL_TEXTURE_2D,0,GL_DEPTH_STENCIL,GL_UNSIGNED_INT_24_8,packed);
    void *other_pixels=NULL;uint64_t current_gen=0;pthread_mutex_lock(&generation_lock);{struct texture_generation *e=generation_entry(c->group,r->name,0);if(e)current_gen=e->gen;}pthread_mutex_unlock(&generation_lock);
    if(other&&current_gen&&other->metal_gen==current_gen){other_pixels=malloc(resident_bytes(other));struct lr_gpu_resident oh={.command=LR_GPU_RES_READ,.id=other->id};if(other_pixels&&!resident_call(c,&oh,other_pixels,resident_bytes(other))){free(other_pixels);other_pixels=NULL;}}
    for(size_t i=0;i<n;i++){float d=r->part==RESIDENT_DEPTH?((float *)pixels)[i]:other_pixels?((float *)other_pixels)[i]:-1;unsigned st=r->part==RESIDENT_STENCIL?((unsigned char *)pixels)[i]:other_pixels?((unsigned char *)other_pixels)[i]:256;
     uint32_t depth24=d<0?(packed[i]>>8):depth_unorm24(d);packed[i]=(depth24<<8)|(st>255?(packed[i]&255):st);}
    sub(GL_TEXTURE_2D,0,0,0,r->width,r->height,GL_DEPTH_STENCIL,GL_UNSIGNED_INT_24_8,packed);free(packed);free(other_pixels);}
  }
  glBindTexture(GL_TEXTURE_2D,prior);
 }
 pixel_end(&unpack,0);free(pixels);s->writebacks++;report_add(r->name?REPORT_WRITEBACK:REPORT_WRITEBACK_WINDOW,started,bytes);report_reasonf("wbfmt:%x-%x-%ux%u",r->internal,r->part,r->width,r->height);return 1;
}

// Bring Mesa up to date for every texture this context holds newer in Metal.
// Must run in the owning context (its Metal copies live on its device file).
static void resident_sync(struct bridge_context *c,GLuint only_name,int only){
 struct resident_state *s=c->resident;if(!s)return;
 GLenum pending=resident_errors_begin();resident_reentry++;
 // A single-texture sync keeps the bound pass unless that texture is one of
 // its attachments (the next draw would otherwise target stale contents).
 int unbind=s->bound&&!only;
 for(unsigned i=0;s->bound&&only&&i<s->bound_count;i++)if(s->bound_names[i]==only_name)unbind=1;
 if(unbind){struct lr_gpu_resident b={.command=LR_GPU_RES_BIND};resident_call(c,&b,NULL,0);s->bound=0;}
 for(unsigned i=0;i<s->limit;i++){struct resident *r=&s->record[i];if(!r->in_use||(only&&r->name!=only_name))continue;
  pthread_mutex_lock(&generation_lock);struct texture_generation *g=generation_entry(resident_group(c,r->name),r->name,0);
  int dirty=g&&r->metal_gen==g->gen&&g->mesa_gen!=g->gen;uint64_t wrote=g?g->gen:0;pthread_mutex_unlock(&generation_lock);
  if(!dirty)continue;
  // Entries can move (deletion shifts them): look the key up again after the transfer.
  if(resident_writeback(c,r)){pthread_mutex_lock(&generation_lock);g=generation_entry(resident_group(c,r->name),r->name,0);if(g&&g->gen==wrote)g->mesa_gen=wrote;pthread_mutex_unlock(&generation_lock);s->syncs++;}
  else note("resident_writeback_failed name=%u part=%d",r->name,r->part);
 }
 resident_reentry--;resident_errors_end(c,pending);
}
// Mesa wrote this texture: Metal copies everywhere are stale; Mesa is current.
static void resident_mesa_wrote(struct bridge_context *c,GLuint name){
 pthread_mutex_lock(&generation_lock);struct texture_generation *g=generation_entry(resident_group(c,name),name,1);
 if(g){g->gen=++generation_clock;g->mesa_gen=g->gen;}pthread_mutex_unlock(&generation_lock);
}
// Make a record current in Metal (upload from Mesa if Metal is behind).
static int resident_ensure(struct bridge_context *c,struct resident *r){
 pthread_mutex_lock(&generation_lock);struct texture_generation *g=generation_entry(resident_group(c,r->name),r->name,1);
 uint64_t gen=g?g->gen:0;int stale=g&&r->metal_gen!=g->gen,mesa_current=g&&g->mesa_gen==g->gen;pthread_mutex_unlock(&generation_lock);
 if(!g)return 0;if(!stale)return 1;
 // Another part of this texture may be newer in this context's Metal copy
 // (a depth write leaves the stencil part behind): bring Mesa up to date first.
 if(!mesa_current){resident_sync(c,r->name,1);pthread_mutex_lock(&generation_lock);g=generation_entry(resident_group(c,r->name),r->name,1);gen=g?g->gen:0;mesa_current=g&&g->mesa_gen==g->gen;pthread_mutex_unlock(&generation_lock);}
 // Newer data only in another context's Metal copy would violate the sync
 // points above; never upload stale Mesa data as current.
 if(!mesa_current){note("resident_fallback reason=foreign-dirty name=%u",r->name);return 0;}
 return resident_upload(c,r,gen);
}
// A Metal draw or clear wrote r: Metal current, Mesa behind.
static void resident_metal_wrote(struct bridge_context *c,struct resident *r){
 pthread_mutex_lock(&generation_lock);struct texture_generation *g=generation_entry(resident_group(c,r->name),r->name,1);
 uint64_t before=0;if(g){if(r->metal_gen!=g->gen)note("resident_write_without_current name=%u",r->name);before=g->gen;g->gen=++generation_clock;r->metal_gen=g->gen;}pthread_mutex_unlock(&generation_lock);
 // The other part of a packed depth/stencil texture shares the generation.
 struct resident *other=r->part==RESIDENT_COLOR?NULL:resident_find(c->resident,r->name,r->part==RESIDENT_DEPTH?RESIDENT_STENCIL:RESIDENT_DEPTH);
 if(other&&g&&before&&other->metal_gen==before)other->metal_gen=r->metal_gen;
}

// ---- render targets --------------------------------------------------------
struct resident_targets {struct resident *color[4],*depth,*stencil;unsigned count;};
// Resolve the bound draw framebuffer's attachments to resident records.
static int resident_targets(struct bridge_context *c,GLint fb,unsigned w,unsigned h,int need_depth,int need_stencil,struct resident_targets *out){
 memset(out,0,sizeof *out);
 for(unsigned i=0;i<c->target_count;i++){struct color_target t=c->targets[i];GLuint name=0;GLint internal=GL_RGBA8;
  if(fb){GLint kind=0,object=0,level=0,layered=0;glGetFramebufferAttachmentParameteriv(GL_DRAW_FRAMEBUFFER,t.buffer,GL_FRAMEBUFFER_ATTACHMENT_OBJECT_TYPE,&kind);
   if(kind!=GL_TEXTURE){report_reasonf("rt:color-object-%x",kind);return 0;}glGetFramebufferAttachmentParameteriv(GL_DRAW_FRAMEBUFFER,t.buffer,GL_FRAMEBUFFER_ATTACHMENT_OBJECT_NAME,&object);
   glGetFramebufferAttachmentParameteriv(GL_DRAW_FRAMEBUFFER,t.buffer,GL_FRAMEBUFFER_ATTACHMENT_TEXTURE_LEVEL,&level);glGetFramebufferAttachmentParameteriv(GL_DRAW_FRAMEBUFFER,t.buffer,GL_FRAMEBUFFER_ATTACHMENT_LAYERED,&layered);
   GLint target=0;glGetTextureParameteriv(object,GL_TEXTURE_TARGET,&target);
   // Some contexts answer 0 here: a non-cube, non-layered level-0 attachment that
   // binds as GL_TEXTURE_2D is a 2D texture (binding another target fails).
   if(!target&&!level&&!layered){GLint face=0;glGetFramebufferAttachmentParameteriv(GL_DRAW_FRAMEBUFFER,t.buffer,GL_FRAMEBUFFER_ATTACHMENT_TEXTURE_CUBE_MAP_FACE,&face);
    if(!face){typeof(&glGetError) error=original("glGetError");if(error)while(error()!=GL_NO_ERROR){}GLint prior=0;glGetIntegerv(GL_TEXTURE_BINDING_2D,&prior);glBindTexture(GL_TEXTURE_2D,object);if(!error||error()==GL_NO_ERROR)target=GL_TEXTURE_2D;glBindTexture(GL_TEXTURE_2D,prior);}}
   if(level||layered||target!=GL_TEXTURE_2D){report_reasonf("rt:color-level%d-layer%d-target%x",level,layered,target);return 0;}GLint tw,th;if(!texture_level(object,&internal,&tw,&th)||(unsigned)tw!=w||(unsigned)th!=h){report_reasonf("rt:color-size");return 0;}name=object;}
  else if(t.buffer!=GL_BACK){report_reasonf("rt:window-buffer-%x",t.buffer);return 0;}
  else if(c->framebuffer_srgb){report_reasonf("rt:window-srgb");return 0;}   // window storage is linear RGBA8
  unsigned format,channels;GLenum type;if(!colour_format(internal,&format,&type,&channels)){report_reasonf("rt:color-format-%x",internal);return 0;}
  if(format!=t.format&&!(t.format==GL_RGBA8&&(format==0x8058||format==LR_GPU_RES_FORMAT_R8||format==LR_GPU_RES_FORMAT_RG8||format==LR_GPU_RES_FORMAT_SRGB8_A8))){report_reasonf("rt:format-disagree-%x-%x",format,t.format);return 0;}   // bridge-detected format must agree
  struct resident *r=resident_record(c,name,RESIDENT_COLOR,w,h,format,internal,type,channels);if(!r||!resident_ensure(c,r)){report_reasonf("rt:color-record");return 0;}out->color[i]=r;
 }
 out->count=c->target_count;
 if((need_depth||need_stencil)&&!fb){
  // Window depth (no window stencil yet): the visual must have a depth buffer.
  if(need_stencil){report_reasonf("rt:window-stencil");return 0;}
  GLint bits=0;glGetFramebufferAttachmentParameteriv(GL_DRAW_FRAMEBUFFER,GL_DEPTH,GL_FRAMEBUFFER_ATTACHMENT_DEPTH_SIZE,&bits);
  if(bits<16){report_reasonf("rt:window-no-depth-%d",bits);return 0;}
  out->depth=resident_record(c,RESIDENT_WINDOW_DEPTH,RESIDENT_DEPTH,w,h,LR_GPU_RES_FORMAT_DEPTH,GL_DEPTH_COMPONENT24,GL_FLOAT,0);
  if(!out->depth||!resident_ensure(c,out->depth)){report_reasonf("rt:window-depth-record");return 0;}
  return 1;
 }
 if(need_depth||need_stencil){GLint kind=0,object=0;
  glGetFramebufferAttachmentParameteriv(GL_DRAW_FRAMEBUFFER,need_depth?GL_DEPTH_ATTACHMENT:GL_STENCIL_ATTACHMENT,GL_FRAMEBUFFER_ATTACHMENT_OBJECT_TYPE,&kind);
  if(kind!=GL_TEXTURE){report_reasonf("rt:depth-object-%x",kind);return 0;}glGetFramebufferAttachmentParameteriv(GL_DRAW_FRAMEBUFFER,need_depth?GL_DEPTH_ATTACHMENT:GL_STENCIL_ATTACHMENT,GL_FRAMEBUFFER_ATTACHMENT_OBJECT_NAME,&object);
  GLint internal,tw,th;if(!texture_level(object,&internal,&tw,&th)||(unsigned)tw!=w||(unsigned)th!=h||!depth_internal(internal)){report_reasonf("rt:depth-format-%x",internal);return 0;}
  if(need_stencil&&!packed_depth_stencil(internal)){report_reasonf("rt:stencil-unpacked-%x",internal);return 0;}
  if(need_depth){out->depth=resident_record(c,object,RESIDENT_DEPTH,w,h,LR_GPU_RES_FORMAT_DEPTH,internal,GL_FLOAT,0);if(!out->depth||!resident_ensure(c,out->depth))return 0;}
  if(need_stencil){out->stencil=resident_record(c,object,RESIDENT_STENCIL,w,h,LR_GPU_RES_FORMAT_STENCIL,internal,GL_UNSIGNED_BYTE,0);if(!out->stencil||!resident_ensure(c,out->stencil))return 0;}
 }
 return 1;
}
static int resident_bind(struct bridge_context *c,const struct resident_targets *t,const unsigned masks[4]){
 struct lr_gpu_resident b={.command=LR_GPU_RES_BIND,.count=t->count,.depth=t->depth?t->depth->id:0,.stencil=t->stencil?t->stencil->id:0,.flags=c->framebuffer_srgb?LR_GPU_RES_FLAG_SRGB:0};
 for(unsigned i=0;i<t->count;i++){b.color[i]=t->color[i]->id;b.masks[i]=masks[i];}
 if(!resident_call(c,&b,NULL,0))return 0;
 struct resident_state *s=resident_state(c);s->bound=1;s->bound_count=0;
 for(unsigned i=0;i<t->count;i++)s->bound_names[s->bound_count++]=t->color[i]->name;
 if(t->depth)s->bound_names[s->bound_count++]=t->depth->name;
 if(t->stencil)s->bound_names[s->bound_count++]=t->stencil->name;
 return 1;
}
static void resident_unbind(struct bridge_context *c){
 struct resident_state *s=c->resident;if(!s||!s->bound)return;struct lr_gpu_resident b={.command=LR_GPU_RES_BIND};resident_call(c,&b,NULL,0);s->bound=0;
}

// Sampled texture on unit -> resident sampler value (base+id), 0 if not eligible.
static unsigned resident_sampler(struct bridge_context *c,GLint unit,const struct resident_targets *targets){
 if(unit<0)return 0;GLint prior,texture=0,sampler=0;glGetIntegerv(GL_ACTIVE_TEXTURE,&prior);glActiveTexture(GL_TEXTURE0+unit);glGetIntegerv(GL_TEXTURE_BINDING_2D,&texture);glGetIntegeri_v(GL_SAMPLER_BINDING,unit,&sampler);
 unsigned value=0;GLint internal=0,w=0,h=0,min=0,mag=0,ws=0,wt=0,base=0,compare=GL_NONE,swizzle[4]={0},mode=GL_DEPTH_COMPONENT,decode=GL_DECODE_EXT;
 if(!texture){report_reasonf("rs:no-2d-texture");goto done;}
 struct resident_state *rs=resident_state(c);uint64_t epoch=atomic_load_explicit(&texture_epoch,memory_order_relaxed);
 struct texture_query *tq=rs&&!atomic_load_explicit(&texture_epoch_unsafe,memory_order_relaxed)?&rs->texture_queries[((unsigned)texture*7u+(unsigned)sampler)%32u]:NULL;
 if(tq&&tq->epoch==epoch&&tq->texture==(GLuint)texture&&tq->sampler==(GLuint)sampler){internal=tq->internal;w=tq->w;h=tq->h;base=tq->base;min=tq->min;mag=tq->mag;ws=tq->ws;wt=tq->wt;compare=tq->compare;mode=tq->mode;decode=tq->decode;memcpy(swizzle,tq->swizzle,sizeof swizzle);}
 else{
 glGetTexLevelParameteriv(GL_TEXTURE_2D,0,GL_TEXTURE_INTERNAL_FORMAT,&internal);glGetTexLevelParameteriv(GL_TEXTURE_2D,0,GL_TEXTURE_WIDTH,&w);glGetTexLevelParameteriv(GL_TEXTURE_2D,0,GL_TEXTURE_HEIGHT,&h);
 glGetTexParameteriv(GL_TEXTURE_2D,GL_TEXTURE_BASE_LEVEL,&base);glGetTexParameteriv(GL_TEXTURE_2D,GL_TEXTURE_SWIZZLE_RGBA,swizzle);
 if(sampler){glGetSamplerParameteriv(sampler,GL_TEXTURE_MIN_FILTER,&min);glGetSamplerParameteriv(sampler,GL_TEXTURE_MAG_FILTER,&mag);glGetSamplerParameteriv(sampler,GL_TEXTURE_WRAP_S,&ws);glGetSamplerParameteriv(sampler,GL_TEXTURE_WRAP_T,&wt);glGetSamplerParameteriv(sampler,GL_TEXTURE_COMPARE_MODE,&compare);}
 else{glGetTexParameteriv(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,&min);glGetTexParameteriv(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,&mag);glGetTexParameteriv(GL_TEXTURE_2D,GL_TEXTURE_WRAP_S,&ws);glGetTexParameteriv(GL_TEXTURE_2D,GL_TEXTURE_WRAP_T,&wt);glGetTexParameteriv(GL_TEXTURE_2D,GL_TEXTURE_COMPARE_MODE,&compare);}
 if(packed_depth_stencil(internal))glGetTexParameteriv(GL_TEXTURE_2D,GL_DEPTH_STENCIL_TEXTURE_MODE,&mode);
 if(internal==GL_SRGB8_ALPHA8){if(sampler)glGetSamplerParameteriv(sampler,GL_TEXTURE_SRGB_DECODE_EXT,&decode);else glGetTexParameteriv(GL_TEXTURE_2D,GL_TEXTURE_SRGB_DECODE_EXT,&decode);}
 if(tq)*tq=(struct texture_query){.texture=texture,.sampler=sampler,.epoch=epoch,.internal=internal,.w=w,.h=h,.base=base,.min=min,.mag=mag,.ws=ws,.wt=wt,.compare=compare,.mode=mode,.decode=decode,.swizzle={swizzle[0],swizzle[1],swizzle[2],swizzle[3]}};
 }
 unsigned format,channels;GLenum type;
 // Depth textures sampled as plain values (no compare, depth not stencil) read
 // an R32Float copy of the resident depth image.
 int depth=depth_internal(internal);
 if(depth){
  if(mode!=GL_DEPTH_COMPONENT){report_reasonf("rs:depth-stencil-mode");goto done;}
  format=LR_GPU_RES_FORMAT_DEPTH;type=GL_FLOAT;channels=1;}
 if(w<1||h<1||w>(GLint)LR_GPU_RESIDENT_MAX_DIMENSION||h>(GLint)LR_GPU_RESIDENT_MAX_DIMENSION||base||compare!=GL_NONE||(!depth&&(!colour_format(internal,&format,&type,&channels)||integer_type(type)))){report_reasonf("rs:format-%x-%dx%d-base%d-cmp%x",internal,w,h,base,compare);goto done;}
 if(swizzle[0]!=GL_RED||swizzle[1]!=GL_GREEN||swizzle[2]!=GL_BLUE||swizzle[3]!=GL_ALPHA){report_reasonf("rs:swizzle-%x",internal);goto done;}
 // Mipmap minification: Metal generates the chain from level 0 (application-supplied
 // lower levels are not uploaded; for box-filtered chains the result is the same).
 int mip=min==GL_NEAREST_MIPMAP_NEAREST||min==GL_LINEAR_MIPMAP_NEAREST||min==GL_NEAREST_MIPMAP_LINEAR||min==GL_LINEAR_MIPMAP_LINEAR;
 if((min!=GL_NEAREST&&min!=GL_LINEAR&&!(mip&&!depth))||(mag!=GL_NEAREST&&mag!=GL_LINEAR)){report_reasonf("rs:filter-%x-%x",min,mag);goto done;}
 // Desktop GL (and Metal) repeat and mirror non-power-of-two textures: no ES completeness rule here.
 struct resident *r=resident_record(c,texture,depth?RESIDENT_DEPTH:RESIDENT_COLOR,w,h,format,internal,type,channels);if(!r)goto done;
 if(depth&&(targets->depth==r||(targets->stencil&&targets->stencil->name==r->name))){note("resident_fallback reason=depth-feedback name=%d",texture);report_reasonf("rs:feedback");goto done;}
 for(unsigned i=0;i<targets->count;i++)if(targets->color[i]==r){note("resident_fallback reason=feedback name=%d",texture);report_reasonf("rs:feedback");goto done;}
 if(!resident_ensure(c,r))goto done;
 struct lr_gpu_resident q={.command=LR_GPU_RES_SAMPLER,.id=r->id,.min_filter=min,.mag_filter=mag,.wrap_s=ws,.wrap_t=wt,.flags=decode==GL_SKIP_DECODE_EXT?LR_GPU_RES_FLAG_RAW:0};
 if(resident_call(c,&q,NULL,0))value=LR_GPU_RESIDENT_SAMPLER_BASE+r->id;
 done:glActiveTexture(prior);return value;
}

// ---- clears on the GPU ----------------------------------------------------
// Whole-attachment clears of resident-eligible targets run on Metal; anything
// partial (scissor, masks) goes to Mesa after a sync.
static int resident_clear(struct bridge_context *c,GLbitfield mask){
 GLint fb;glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING,&fb);
 if(glIsEnabled(GL_RASTERIZER_DISCARD))return 0;
 // Write masks and the scissor box bound a clear. Full clears use Metal
 // load-clears; others a scissored, write-masked clear pass (no transfers).
 unsigned colour_mask[4]={15,15,15,15};int partial=0;
 if(mask&GL_COLOR_BUFFER_BIT)for(unsigned i=0;i<4;i++){GLboolean m[4];glGetBooleani_v(GL_COLOR_WRITEMASK,i,m);colour_mask[i]=(m[0]?1:0)|(m[1]?2:0)|(m[2]?4:0)|(m[3]?8:0);if(colour_mask[i]!=15)partial=1;}
 if(mask&GL_DEPTH_BUFFER_BIT){GLboolean m;glGetBooleanv(GL_DEPTH_WRITEMASK,&m);if(!m)mask&=~GL_DEPTH_BUFFER_BIT;}
 GLint stencil_mask=255;if(mask&GL_STENCIL_BUFFER_BIT){glGetIntegerv(GL_STENCIL_WRITEMASK,&stencil_mask);stencil_mask&=255;if(!stencil_mask)mask&=~GL_STENCIL_BUFFER_BIT;else if(stencil_mask!=255)partial=1;}
 if(!(mask&(GL_COLOR_BUFFER_BIT|GL_DEPTH_BUFFER_BIT|GL_STENCIL_BUFFER_BIT)))return 1;   // everything masked: GL writes nothing
 unsigned w=0,h=0;if(!collect_targets(c,fb,&w,&h))return 0;
 int x0=0,y0=0,x1=(int)w,y1=(int)h;
 if(glIsEnabled(GL_SCISSOR_TEST)){GLint box[4];glGetIntegerv(GL_SCISSOR_BOX,box);x0=box[0]>0?box[0]:0;y0=box[1]>0?box[1]:0;
  long rx=(long)box[0]+box[2],ry=(long)box[1]+box[3];x1=rx<(long)w?(int)rx:(int)w;y1=ry<(long)h?(int)ry:(int)h;partial=1;}
 if(x1<=x0||y1<=y0)return 1;   // empty scissor
 struct resident_targets t;if(!resident_targets(c,fb,w,h,!!(mask&GL_DEPTH_BUFFER_BIT),!!(mask&GL_STENCIL_BUFFER_BIT),&t))return 0;
 if(!(mask&GL_COLOR_BUFFER_BIT))t.count=0;
 float color[4];GLint bits[4];GLfloat depth;GLint stencil;glGetFloatv(GL_COLOR_CLEAR_VALUE,color);glGetIntegerv(GL_COLOR_CLEAR_VALUE,bits);glGetFloatv(GL_DEPTH_CLEAR_VALUE,&depth);glGetIntegerv(GL_STENCIL_CLEAR_VALUE,&stencil);
 struct resident *list[6];unsigned masks[6],n=0;for(unsigned i=0;i<t.count;i++){masks[n]=colour_mask[i]&t.color[i]->channels;list[n++]=t.color[i];}if(t.depth){masks[n]=0;list[n++]=t.depth;}if(t.stencil){masks[n]=0;list[n++]=t.stencil;}
 for(unsigned i=0;i<n;i++){struct resident *r=list[i];
  // A masked-off colour attachment is untouched; a channel the format lacks is not a partial mask.
  if(r->part==RESIDENT_COLOR&&!masks[i])continue;
  int region=partial&&!(r->part==RESIDENT_COLOR&&masks[i]==r->channels&&!glIsEnabled(GL_SCISSOR_TEST))&&!(r->part==RESIDENT_DEPTH&&!glIsEnabled(GL_SCISSOR_TEST))&&!(r->part==RESIDENT_STENCIL&&stencil_mask==255&&!glIsEnabled(GL_SCISSOR_TEST));
  struct lr_gpu_resident q={.command=LR_GPU_RES_CLEAR,.id=r->id,.clear_depth=depth,.clear_stencil=(unsigned)stencil&255,.flags=(c->framebuffer_srgb?LR_GPU_RES_FLAG_SRGB:0)|(region?LR_GPU_RES_FLAG_REGION:0),
   .x=(unsigned)x0,.y=(unsigned)y0,.width=(unsigned)(x1-x0),.height=(unsigned)(y1-y0),.masks={r->part==RESIDENT_COLOR?(masks[i]|(15&~r->channels)):0,(unsigned)stencil_mask}};
  for(unsigned k=0;k<4;k++){int present=(r->channels>>k)&1;q.clear[k]=present?color[k]:(k==3);q.clear_bits[k]=present?(uint32_t)bits[k]:(k==3);}
  // A region clear keeps the rest of the target: Metal must hold current contents first.
  if(region&&!resident_ensure(c,r))return 0;
  if(!resident_call(c,&q,NULL,0))return 0;resident_metal_wrote(c,r);}
 resident_state(c)->gpu_clears++;return 1;
}

// ---- hooks -------------------------------------------------------------------
static struct bridge_context *resident_context(void){
 if(resident_reentry||!resident_enabled())return NULL;struct bridge_context *c=current_context();
 // Every context tracks Mesa writes (other contexts may hold Metal copies);
 // only contexts with resident state have anything to sync.
 return c&&c->owner==current_tid()?c:NULL;
}
static GLuint bound_texture(GLenum target){GLint t=0;if(target==GL_TEXTURE_2D)glGetIntegerv(GL_TEXTURE_BINDING_2D,&t);else return 0;return (GLuint)t;}
// Attachments of the bound draw framebuffer that a Mesa write would change.
static void resident_mesa_wrote_framebuffer(struct bridge_context *c){
 GLenum pending=resident_errors_begin();GLint fb;glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING,&fb);
 if(!fb){resident_mesa_wrote(c,0);resident_mesa_wrote(c,RESIDENT_WINDOW_DEPTH);resident_errors_end(c,pending);return;}
 GLenum points[]={GL_COLOR_ATTACHMENT0,GL_COLOR_ATTACHMENT1,GL_COLOR_ATTACHMENT2,GL_COLOR_ATTACHMENT3,GL_DEPTH_ATTACHMENT,GL_STENCIL_ATTACHMENT};
 for(unsigned i=0;i<6;i++){GLint kind=0,object=0;glGetFramebufferAttachmentParameteriv(GL_DRAW_FRAMEBUFFER,points[i],GL_FRAMEBUFFER_ATTACHMENT_OBJECT_TYPE,&kind);
  if(kind==GL_TEXTURE){glGetFramebufferAttachmentParameteriv(GL_DRAW_FRAMEBUFFER,points[i],GL_FRAMEBUFFER_ATTACHMENT_OBJECT_NAME,&object);resident_mesa_wrote(c,(GLuint)object);}}
 resident_errors_end(c,pending);
}
// GL updates the depth buffer only with the depth test enabled and depth
// writes on, and the stencil buffer only with the stencil test enabled and a
// nonzero write mask; with a test disabled the buffer is neither read nor
// written. Fallback draws use this to leave depth/stencil on the GPU (Mesa's
// depth upload alone costs ~10 ms per viewport-sized image).
static int mesa_draw_reads_depth(void){return glIsEnabled(GL_DEPTH_TEST);}
static int mesa_draw_writes_depth(void){GLboolean m=GL_FALSE;if(!glIsEnabled(GL_DEPTH_TEST))return 0;glGetBooleanv(GL_DEPTH_WRITEMASK,&m);return m;}
static int mesa_draw_uses_stencil(void){return glIsEnabled(GL_STENCIL_TEST);}
static int mesa_draw_writes_stencil(void){GLint f=0,b=0;if(!glIsEnabled(GL_STENCIL_TEST))return 0;glGetIntegerv(GL_STENCIL_WRITEMASK,&f);glGetIntegerv(GL_STENCIL_BACK_WRITEMASK,&b);return ((f|b)&255)!=0;}
// After a Mesa draw: the attachments it could have changed.
static void resident_mesa_draw_wrote(struct bridge_context *c,const GLuint *skip,unsigned skips){
 GLenum pending=resident_errors_begin();GLint fb;glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING,&fb);
 if(!fb){int planned=0;for(unsigned k=0;k<skips;k++)if(!skip[k])planned=1;if(!planned)resident_mesa_wrote(c,0);
  int depth_planned=0;for(unsigned k=0;k<skips;k++)if(skip[k]==RESIDENT_WINDOW_DEPTH)depth_planned=1;if(!depth_planned&&mesa_draw_writes_depth())resident_mesa_wrote(c,RESIDENT_WINDOW_DEPTH);
  resident_errors_end(c,pending);return;}
 int depth=mesa_draw_writes_depth(),stencil=mesa_draw_writes_stencil();
 GLenum points[]={GL_COLOR_ATTACHMENT0,GL_COLOR_ATTACHMENT1,GL_COLOR_ATTACHMENT2,GL_COLOR_ATTACHMENT3,GL_COLOR_ATTACHMENT4,GL_COLOR_ATTACHMENT5,GL_COLOR_ATTACHMENT6,GL_COLOR_ATTACHMENT7,GL_DEPTH_ATTACHMENT,GL_STENCIL_ATTACHMENT};
 for(unsigned i=0;i<10;i++){if((points[i]==GL_DEPTH_ATTACHMENT&&!depth)||(points[i]==GL_STENCIL_ATTACHMENT&&!stencil))continue;
  GLint kind=0,object=0;glGetFramebufferAttachmentParameteriv(GL_DRAW_FRAMEBUFFER,points[i],GL_FRAMEBUFFER_ATTACHMENT_OBJECT_TYPE,&kind);if(kind!=GL_TEXTURE)continue;
  glGetFramebufferAttachmentParameteriv(GL_DRAW_FRAMEBUFFER,points[i],GL_FRAMEBUFFER_ATTACHMENT_OBJECT_NAME,&object);
  int planned=0;for(unsigned k=0;k<skips;k++)if(skip[k]==(GLuint)object)planned=1;if(!planned)resident_mesa_wrote(c,(GLuint)object);}
 resident_errors_end(c,pending);
}
#define RESIDENT_SYNC_ALL(c) do{if(c){report_reasonf("sync:%s",__func__);resident_sync(c,0,0);}}while(0)
static void resident_sync_framebuffer(struct bridge_context *c,GLenum binding);
// Another live context shares this one's textures: Metal-only contents must be
// made visible through Mesa at flush points (glFlush, glFinish, fences, swap).
// With no such context nothing else can observe them, so flush points skip it.
static int resident_group_shared(struct bridge_context *c){
 int others=0;pthread_mutex_lock(&records);
 for(unsigned i=0;i<CONTEXTS;i++)if(&contexts[i]!=c&&contexts[i].handle&&!contexts[i].deleted&&contexts[i].group==c->group)others=1;
 pthread_mutex_unlock(&records);return others;
}
#define RESIDENT_FLUSH_POINT(c) do{if((c)&&resident_group_shared(c)){report_reasonf("sync:%s",__func__);resident_sync(c,0,0);}}while(0)
// Only what an operation reads or writes is brought up to date in Mesa.
#define RESIDENT_SYNC_FB(c,binding) do{if(c){report_reasonf("sync:%s",__func__);GLenum p_=resident_errors_begin();resident_reentry++;resident_sync_framebuffer(c,binding);resident_reentry--;resident_errors_end(c,p_);}}while(0)
#define RESIDENT_SYNC_NAME(c,name) do{if((c)&&(name)){report_reasonf("sync:%s",__func__);resident_sync(c,(GLuint)(name),1);}}while(0)
#define HOOK_ORIGINAL(name) typeof(&name) real=original(#name);if(!real)return

void glClear(GLbitfield mask){HOOK_ORIGINAL(glClear);struct bridge_context *c=resident_context();
 if(c){GLenum pending=resident_errors_begin();resident_reentry++;int done=resident_clear(c,mask);resident_reentry--;resident_errors_end(c,pending);if(done)return;RESIDENT_SYNC_FB(c,GL_DRAW_FRAMEBUFFER_BINDING);}
 real(mask);if(c)resident_mesa_wrote_framebuffer(c);}
void glClearBufferfv(GLenum buffer,GLint index,const GLfloat *value){HOOK_ORIGINAL(glClearBufferfv);struct bridge_context *c=resident_context();RESIDENT_SYNC_FB(c,GL_DRAW_FRAMEBUFFER_BINDING);real(buffer,index,value);if(c)resident_mesa_wrote_framebuffer(c);}
void glClearBufferiv(GLenum buffer,GLint index,const GLint *value){HOOK_ORIGINAL(glClearBufferiv);struct bridge_context *c=resident_context();RESIDENT_SYNC_FB(c,GL_DRAW_FRAMEBUFFER_BINDING);real(buffer,index,value);if(c)resident_mesa_wrote_framebuffer(c);}
void glClearBufferuiv(GLenum buffer,GLint index,const GLuint *value){HOOK_ORIGINAL(glClearBufferuiv);struct bridge_context *c=resident_context();RESIDENT_SYNC_FB(c,GL_DRAW_FRAMEBUFFER_BINDING);real(buffer,index,value);if(c)resident_mesa_wrote_framebuffer(c);}
void glClearBufferfi(GLenum buffer,GLint index,GLfloat depth,GLint stencil){HOOK_ORIGINAL(glClearBufferfi);struct bridge_context *c=resident_context();RESIDENT_SYNC_FB(c,GL_DRAW_FRAMEBUFFER_BINDING);real(buffer,index,depth,stencil);if(c)resident_mesa_wrote_framebuffer(c);}
void glReadPixels(GLint x,GLint y,GLsizei w,GLsizei h,GLenum format,GLenum type,void *data){HOOK_ORIGINAL(glReadPixels);struct bridge_context *c=resident_context();RESIDENT_SYNC_FB(c,GL_READ_FRAMEBUFFER_BINDING);real(x,y,w,h,format,type,data);}
void glReadnPixels(GLint x,GLint y,GLsizei w,GLsizei h,GLenum format,GLenum type,GLsizei size,void *data){HOOK_ORIGINAL(glReadnPixels);struct bridge_context *c=resident_context();RESIDENT_SYNC_FB(c,GL_READ_FRAMEBUFFER_BINDING);real(x,y,w,h,format,type,size,data);}
void glGetTexImage(GLenum target,GLint level,GLenum format,GLenum type,void *pixels){HOOK_ORIGINAL(glGetTexImage);struct bridge_context *c=resident_context();RESIDENT_SYNC_NAME(c,target==GL_TEXTURE_2D?bound_texture(target):0);real(target,level,format,type,pixels);}
void glGetTextureImage(GLuint texture,GLint level,GLenum format,GLenum type,GLsizei size,void *pixels){HOOK_ORIGINAL(glGetTextureImage);struct bridge_context *c=resident_context();RESIDENT_SYNC_NAME(c,texture);real(texture,level,format,type,size,pixels);}
// Attachments of a bound framebuffer: what a Mesa copy can read or write.
static void resident_sync_framebuffer(struct bridge_context *c,GLenum binding){
 GLint fb=0;glGetIntegerv(binding,&fb);if(!fb){resident_sync(c,0,1);resident_sync(c,RESIDENT_WINDOW_DEPTH,1);return;}
 GLenum target=binding==GL_READ_FRAMEBUFFER_BINDING?GL_READ_FRAMEBUFFER:GL_DRAW_FRAMEBUFFER;
 GLenum points[]={GL_COLOR_ATTACHMENT0,GL_COLOR_ATTACHMENT1,GL_COLOR_ATTACHMENT2,GL_COLOR_ATTACHMENT3,GL_COLOR_ATTACHMENT4,GL_COLOR_ATTACHMENT5,GL_COLOR_ATTACHMENT6,GL_COLOR_ATTACHMENT7,GL_DEPTH_ATTACHMENT,GL_STENCIL_ATTACHMENT};
 for(unsigned i=0;i<10;i++){GLint kind=0,object=0;glGetFramebufferAttachmentParameteriv(target,points[i],GL_FRAMEBUFFER_ATTACHMENT_OBJECT_TYPE,&kind);
  if(kind==GL_TEXTURE){glGetFramebufferAttachmentParameteriv(target,points[i],GL_FRAMEBUFFER_ATTACHMENT_OBJECT_NAME,&object);resident_sync(c,(GLuint)object,1);}}
}
// Resident record for the read framebuffer's read buffer (colour) or depth attachment.
static struct resident *resident_read_image(struct bridge_context *c,int depth,unsigned *w,unsigned *h){
 GLint fb=0;glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING,&fb);
 if(!fb){if(depth)return NULL;GLint buffer=0;glGetIntegerv(GL_READ_BUFFER,&buffer);GLboolean stereo=GL_FALSE;glGetBooleanv(GL_STEREO,&stereo);
  if(buffer!=GL_BACK&&!(buffer==GL_BACK_LEFT&&!stereo))return NULL;
  unsigned ww=0,hh=0;window_size(&ww,&hh);
  GLint alpha=0;glGetFramebufferAttachmentParameteriv(GL_READ_FRAMEBUFFER,GL_BACK_LEFT,GL_FRAMEBUFFER_ATTACHMENT_ALPHA_SIZE,&alpha);
  GLint encoding=GL_LINEAR;glGetFramebufferAttachmentParameteriv(GL_READ_FRAMEBUFFER,GL_BACK_LEFT,GL_FRAMEBUFFER_ATTACHMENT_COLOR_ENCODING,&encoding);
  if(!ww||!hh||(encoding==GL_SRGB&&glIsEnabled(GL_FRAMEBUFFER_SRGB)))return NULL;
  *w=ww;*h=hh;return resident_record(c,0,RESIDENT_COLOR,ww,hh,0x8058,GL_RGBA8,GL_UNSIGNED_BYTE,alpha?15:7);}
 GLenum point=GL_DEPTH_ATTACHMENT;if(!depth){GLint buffer=0;glGetIntegerv(GL_READ_BUFFER,&buffer);if(buffer<GL_COLOR_ATTACHMENT0||buffer>GL_COLOR_ATTACHMENT15)return NULL;point=buffer;}
 GLint kind=0,object=0,level=0,layered=0;glGetFramebufferAttachmentParameteriv(GL_READ_FRAMEBUFFER,point,GL_FRAMEBUFFER_ATTACHMENT_OBJECT_TYPE,&kind);if(kind!=GL_TEXTURE)return NULL;
 glGetFramebufferAttachmentParameteriv(GL_READ_FRAMEBUFFER,point,GL_FRAMEBUFFER_ATTACHMENT_OBJECT_NAME,&object);glGetFramebufferAttachmentParameteriv(GL_READ_FRAMEBUFFER,point,GL_FRAMEBUFFER_ATTACHMENT_TEXTURE_LEVEL,&level);glGetFramebufferAttachmentParameteriv(GL_READ_FRAMEBUFFER,point,GL_FRAMEBUFFER_ATTACHMENT_LAYERED,&layered);
 GLint target=0;glGetTextureParameteriv(object,GL_TEXTURE_TARGET,&target);if(level||layered||target!=GL_TEXTURE_2D)return NULL;
 GLint internal,tw,th;if(!texture_level(object,&internal,&tw,&th))return NULL;*w=tw;*h=th;
 if(depth){if(!depth_internal(internal))return NULL;return resident_record(c,object,RESIDENT_DEPTH,tw,th,LR_GPU_RES_FORMAT_DEPTH,internal,GL_FLOAT,0);}
 unsigned format,channels;GLenum type;if(!colour_format(internal,&format,&type,&channels)||integer_type(type))return NULL;
 return resident_record(c,object,RESIDENT_COLOR,tw,th,format,internal,type,channels);
}
static int resident_blit(struct bridge_context *c,const GLint *rect,GLbitfield mask,GLenum filter){
 if((mask!=GL_COLOR_BUFFER_BIT&&mask!=GL_DEPTH_BUFFER_BIT)||(filter!=GL_NEAREST&&filter!=GL_LINEAR)||(mask==GL_DEPTH_BUFFER_BIT&&filter!=GL_NEAREST))return 0;
 if(glIsEnabled(GL_RASTERIZER_DISCARD))return 0;
 int depth=mask==GL_DEPTH_BUFFER_BIT;unsigned sw=0,sh=0;struct resident *src=resident_read_image(c,depth,&sw,&sh);if(!src||!resident_ensure(c,src))return 0;
 GLint fb=0;glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING,&fb);unsigned w=0,h=0;if(!collect_targets(c,fb,&w,&h)||(!depth&&c->target_count!=1))return 0;
 struct resident_targets t;if(!resident_targets(c,fb,w,h,depth,0,&t))return 0;
 struct resident *dst=depth?t.depth:t.color[0];if(!dst||dst==src||dst->name==src->name)return 0;
 // GL: a blit between sRGB images converts only with GL_FRAMEBUFFER_SRGB.
 struct lr_gpu_resident q={.command=LR_GPU_RES_BLIT,.id=src->id,.color={dst->id},.min_filter=filter,.flags=glIsEnabled(GL_FRAMEBUFFER_SRGB)?LR_GPU_RES_FLAG_SRGB:0};
 for(unsigned i=0;i<8;i++)q.rect[i]=rect[i];
 if(glIsEnabled(GL_SCISSOR_TEST)){GLint box[4];glGetIntegerv(GL_SCISSOR_BOX,box);if(box[2]<=0||box[3]<=0)return 1;
  long x0=box[0]>0?box[0]:0,y0=box[1]>0?box[1]:0,x1=(long)box[0]+box[2],y1=(long)box[1]+box[3];if(x1<=x0||y1<=y0)return 1;
  q.flags|=LR_GPU_RES_FLAG_REGION;q.x=(unsigned)x0;q.y=(unsigned)y0;q.width=(unsigned)(x1-x0);q.height=(unsigned)(y1-y0);}
 if(!resident_call(c,&q,NULL,0))return 0;resident_metal_wrote(c,dst);atomic_fetch_add(&stats_blits,1);return 1;
}
void glBlitFramebuffer(GLint sx0,GLint sy0,GLint sx1,GLint sy1,GLint dx0,GLint dy0,GLint dx1,GLint dy1,GLbitfield mask,GLenum filter){HOOK_ORIGINAL(glBlitFramebuffer);struct bridge_context *c=resident_context();
 if(c){GLint rect[8]={sx0,sy0,sx1,sy1,dx0,dy0,dx1,dy1};GLenum pending=resident_errors_begin();resident_reentry++;int done=resident_blit(c,rect,mask,filter);
  // Not on the GPU: Mesa reads the read framebuffer and writes the draw framebuffer, nothing else.
  if(!done){report_reasonf("sync:blit-fallback");resident_sync_framebuffer(c,GL_READ_FRAMEBUFFER_BINDING);resident_sync_framebuffer(c,GL_DRAW_FRAMEBUFFER_BINDING);}
  resident_reentry--;resident_errors_end(c,pending);if(done)return;}
 real(sx0,sy0,sx1,sy1,dx0,dy0,dx1,dy1,mask,filter);if(c)resident_mesa_wrote_framebuffer(c);}
void glCopyTexSubImage2D(GLenum target,GLint level,GLint xo,GLint yo,GLint x,GLint y,GLsizei w,GLsizei h){HOOK_ORIGINAL(glCopyTexSubImage2D);struct bridge_context *c=resident_context();RESIDENT_SYNC_FB(c,GL_READ_FRAMEBUFFER_BINDING);RESIDENT_SYNC_NAME(c,bound_texture(target));real(target,level,xo,yo,x,y,w,h);if(c)resident_mesa_wrote(c,bound_texture(target));}
void glCopyTexImage2D(GLenum target,GLint level,GLenum internal,GLint x,GLint y,GLsizei w,GLsizei h,GLint border){HOOK_ORIGINAL(glCopyTexImage2D);texture_state_changed();struct bridge_context *c=resident_context();RESIDENT_SYNC_FB(c,GL_READ_FRAMEBUFFER_BINDING);RESIDENT_SYNC_NAME(c,bound_texture(target));real(target,level,internal,x,y,w,h,border);if(c)resident_mesa_wrote(c,bound_texture(target));}
void glCopyImageSubData(GLuint src,GLenum st,GLint sl,GLint sx,GLint sy,GLint sz,GLuint dst,GLenum dt,GLint dl,GLint dx,GLint dy,GLint dz,GLsizei w,GLsizei h,GLsizei d){HOOK_ORIGINAL(glCopyImageSubData);struct bridge_context *c=resident_context();if(st==GL_TEXTURE_2D)RESIDENT_SYNC_NAME(c,src);if(dt==GL_TEXTURE_2D)RESIDENT_SYNC_NAME(c,dst);if(st!=GL_TEXTURE_2D||dt!=GL_TEXTURE_2D)RESIDENT_SYNC_ALL(c);real(src,st,sl,sx,sy,sz,dst,dt,dl,dx,dy,dz,w,h,d);if(c)resident_mesa_wrote(c,dst);}
void glTexSubImage2D(GLenum target,GLint level,GLint x,GLint y,GLsizei w,GLsizei h,GLenum format,GLenum type,const void *pixels){HOOK_ORIGINAL(glTexSubImage2D);struct bridge_context *c=resident_context();GLuint t=c?bound_texture(target):0;if(c&&t)resident_sync(c,t,1);real(target,level,x,y,w,h,format,type,pixels);if(c&&t)resident_mesa_wrote(c,t);}
void glTexImage2D(GLenum target,GLint level,GLint internal,GLsizei w,GLsizei h,GLint border,GLenum format,GLenum type,const void *pixels){HOOK_ORIGINAL(glTexImage2D);texture_state_changed();struct bridge_context *c=resident_context();GLuint t=c?bound_texture(target):0;if(c&&t)resident_sync(c,t,1);real(target,level,internal,w,h,border,format,type,pixels);if(c&&t)resident_mesa_wrote(c,t);}
void glTexStorage2D(GLenum target,GLsizei levels,GLenum internal,GLsizei w,GLsizei h){HOOK_ORIGINAL(glTexStorage2D);texture_state_changed();struct bridge_context *c=resident_context();GLuint t=c?bound_texture(target):0;real(target,levels,internal,w,h);if(c&&t)resident_mesa_wrote(c,t);}
void glTextureSubImage2D(GLuint texture,GLint level,GLint x,GLint y,GLsizei w,GLsizei h,GLenum format,GLenum type,const void *pixels){HOOK_ORIGINAL(glTextureSubImage2D);struct bridge_context *c=resident_context();if(c)resident_sync(c,texture,1);real(texture,level,x,y,w,h,format,type,pixels);if(c)resident_mesa_wrote(c,texture);}
void glGenerateMipmap(GLenum target){HOOK_ORIGINAL(glGenerateMipmap);texture_state_changed();struct bridge_context *c=resident_context();GLuint t=c?bound_texture(target):0;if(c&&t)resident_sync(c,t,1);real(target);if(c&&t)resident_mesa_wrote(c,t);}
void glGenerateTextureMipmap(GLuint texture){HOOK_ORIGINAL(glGenerateTextureMipmap);texture_state_changed();struct bridge_context *c=resident_context();if(c)resident_sync(c,texture,1);real(texture);if(c)resident_mesa_wrote(c,texture);}
void glClearTexImage(GLuint texture,GLint level,GLenum format,GLenum type,const void *data){HOOK_ORIGINAL(glClearTexImage);struct bridge_context *c=resident_context();if(c)resident_sync(c,texture,1);real(texture,level,format,type,data);if(c)resident_mesa_wrote(c,texture);}
void glClearTexSubImage(GLuint texture,GLint level,GLint x,GLint y,GLint z,GLsizei w,GLsizei h,GLsizei d,GLenum format,GLenum type,const void *data){HOOK_ORIGINAL(glClearTexSubImage);struct bridge_context *c=resident_context();if(c)resident_sync(c,texture,1);real(texture,level,x,y,z,w,h,d,format,type,data);if(c)resident_mesa_wrote(c,texture);}
void glDeleteTextures(GLsizei n,const GLuint *textures){HOOK_ORIGINAL(glDeleteTextures);texture_state_changed();struct bridge_context *c=resident_context();
 if(c&&c->resident&&n>0&&textures){resident_unbind(c);for(GLsizei i=0;i<n;i++){for(unsigned j=0;j<c->resident->limit;j++){struct resident *r=&c->resident->record[j];if(r->in_use&&r->name==textures[i])resident_drop(c,r);}
  pthread_mutex_lock(&generation_lock);generation_remove(c->group,textures[i]);pthread_mutex_unlock(&generation_lock);}}
 real(n,textures);}
void glFlush(void){HOOK_ORIGINAL(glFlush);struct bridge_context *c=resident_context();RESIDENT_FLUSH_POINT(c);real();}
void glFinish(void){HOOK_ORIGINAL(glFinish);struct bridge_context *c=resident_context();RESIDENT_FLUSH_POINT(c);real();}
GLsync glFenceSync(GLenum condition,GLbitfield flags){typeof(&glFenceSync) real=original("glFenceSync");if(!real)return 0;struct bridge_context *c=resident_context();RESIDENT_FLUSH_POINT(c);return real(condition,flags);}
static void resident_report(struct bridge_context *c);
// Adaptive retirement: every 30 frames compare draws that fell back to Mesa
// with draws Metal executed, and window write-backs with frames. Sustained
// fallback (over 20% of draws) with Mesa/Metal ping-pong (more than two
// copies, write-backs plus re-uploads, per frame) costs more than Mesa alone:
// sync and retire.
// LR_MESA_METAL_ADAPTIVE=0 keeps the bridge regardless.
static void bridge_consider_retiring(struct bridge_context *c){
 static LR_TLS uint64_t frames,metal_at,mesa_at,writebacks_at,uploads_at;
 if(!c||!c->resident||resident_group_shared(c))return;
 const char *adaptive=config("LR_MESA_METAL_ADAPTIVE");if(adaptive&&!strcmp(adaptive,"0"))return;
 if(++frames%30)return;
 struct resident_state *s=c->resident;
 uint64_t metal=s->draws-metal_at,mesa=s->mesa_draws-mesa_at,wb=(s->writebacks-writebacks_at)+(s->uploads-uploads_at);
 metal_at=s->draws;mesa_at=s->mesa_draws;writebacks_at=s->writebacks;uploads_at=s->uploads;
 if(mesa*5>metal+mesa&&wb>2*30){   // wb: Metal<->Mesa copies (write-backs and re-uploads)
  resident_sync(c,0,0);resident_unbind(c);atomic_store(&bridge_retired,1);
  {struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);atomic_store(&bridge_retired_ns,(uint64_t)t.tv_sec*1000000000ull+(uint64_t)t.tv_nsec);atomic_store(&bridge_retired_frames,atomic_load(&stats_frames));}
  note("bridge_retired frames=%llu metal_draws=%llu mesa_draws=%llu transfers=%llu reason=mixed-rendering-slower-than-mesa",(unsigned long long)frames,(unsigned long long)metal,(unsigned long long)mesa,(unsigned long long)wb);
  report_reason("bridge-retired");
 }
}
// Before a window presentation (GLX or EGL): Metal-resident images of the presented framebuffer return to Mesa.
static void before_present(void){window_size_changed();atomic_fetch_add(&stats_frames,1);if(!resident_reentry)stats_write(0);struct bridge_context *c=resident_context();if(c){if(resident_group_shared(c))RESIDENT_SYNC_ALL(c);else{report_reasonf("sync:swap-window");resident_sync(c,0,1);}}   /* presented image only; other images stay on the GPU */
 if(c&&c->resident){static LR_TLS unsigned swaps;if(++swaps%64==0)resident_report(c);GLenum pending=resident_errors_begin();resident_reentry++;bridge_consider_retiring(c);resident_reentry--;resident_errors_end(c,pending);}
}
void glXSwapBuffers(Display *d,GLXDrawable drawable){HOOK_ORIGINAL(glXSwapBuffers);before_present();real(d,drawable);}
EGLBoolean eglSwapBuffers(EGLDisplay d,EGLSurface s){HOOK_ORIGINAL(eglSwapBuffers) EGL_FALSE;before_present();return real(d,s);}
// Extension entry points: reached through eglGetProcAddress (the provider is recorded there).
EGLBoolean eglSwapBuffersWithDamageKHR(EGLDisplay d,EGLSurface s,const EGLint *rects,EGLint n){HOOK_ORIGINAL(eglSwapBuffersWithDamageKHR) EGL_FALSE;before_present();return real(d,s,rects,n);}
EGLBoolean eglSwapBuffersWithDamageEXT(EGLDisplay d,EGLSurface s,const EGLint *rects,EGLint n){HOOK_ORIGINAL(eglSwapBuffersWithDamageEXT) EGL_FALSE;before_present();return real(d,s,rects,n);}
// Texture/sampler parameter writers (texture_epoch). Pass-through plus an epoch bump.
#define EPOCH_HOOK(name,params,args) void name params{HOOK_ORIGINAL(name);texture_state_changed();real args;}
EPOCH_HOOK(glTexParameteri,(GLenum t,GLenum p,GLint v),(t,p,v))
EPOCH_HOOK(glTexParameterf,(GLenum t,GLenum p,GLfloat v),(t,p,v))
EPOCH_HOOK(glTexParameteriv,(GLenum t,GLenum p,const GLint *v),(t,p,v))
EPOCH_HOOK(glTexParameterfv,(GLenum t,GLenum p,const GLfloat *v),(t,p,v))
EPOCH_HOOK(glTexParameterIiv,(GLenum t,GLenum p,const GLint *v),(t,p,v))
EPOCH_HOOK(glTexParameterIuiv,(GLenum t,GLenum p,const GLuint *v),(t,p,v))
EPOCH_HOOK(glTextureParameteri,(GLuint t,GLenum p,GLint v),(t,p,v))
EPOCH_HOOK(glTextureParameterf,(GLuint t,GLenum p,GLfloat v),(t,p,v))
EPOCH_HOOK(glTextureParameteriv,(GLuint t,GLenum p,const GLint *v),(t,p,v))
EPOCH_HOOK(glTextureParameterfv,(GLuint t,GLenum p,const GLfloat *v),(t,p,v))
EPOCH_HOOK(glSamplerParameteri,(GLuint s,GLenum p,GLint v),(s,p,v))
EPOCH_HOOK(glSamplerParameterf,(GLuint s,GLenum p,GLfloat v),(s,p,v))
EPOCH_HOOK(glSamplerParameteriv,(GLuint s,GLenum p,const GLint *v),(s,p,v))
EPOCH_HOOK(glSamplerParameterfv,(GLuint s,GLenum p,const GLfloat *v),(s,p,v))
EPOCH_HOOK(glSamplerParameterIiv,(GLuint s,GLenum p,const GLint *v),(s,p,v))
EPOCH_HOOK(glSamplerParameterIuiv,(GLuint s,GLenum p,const GLuint *v),(s,p,v))
EPOCH_HOOK(glDeleteSamplers,(GLsizei n,const GLuint *s),(n,s))
EPOCH_HOOK(glCompressedTexImage2D,(GLenum t,GLint l,GLenum f,GLsizei w,GLsizei h,GLint b,GLsizei n,const void *d),(t,l,f,w,h,b,n,d))
EPOCH_HOOK(glTexStorage3D,(GLenum t,GLsizei l,GLenum f,GLsizei w,GLsizei h,GLsizei d),(t,l,f,w,h,d))
EPOCH_HOOK(glTextureStorage2D,(GLuint t,GLsizei l,GLenum f,GLsizei w,GLsizei h),(t,l,f,w,h))
EPOCH_HOOK(glTextureView,(GLuint t,GLenum target,GLuint o,GLenum f,GLuint ml,GLuint nl,GLuint mi,GLuint ni),(t,target,o,f,ml,nl,mi,ni))
EPOCH_HOOK(glPopAttrib,(void),())
EPOCH_HOOK(glCallList,(GLuint l),(l))
EPOCH_HOOK(glCallLists,(GLsizei n,GLenum t,const void *l),(n,t,l))
#undef EPOCH_HOOK
#define EPOCH_ENTRIES ENTRY(glTexParameteri),ENTRY(glTexParameterf),ENTRY(glTexParameteriv),ENTRY(glTexParameterfv),ENTRY(glTexParameterIiv),ENTRY(glTexParameterIuiv),ENTRY(glTextureParameteri),ENTRY(glTextureParameterf),ENTRY(glTextureParameteriv),ENTRY(glTextureParameterfv),ENTRY(glSamplerParameteri),ENTRY(glSamplerParameterf),ENTRY(glSamplerParameteriv),ENTRY(glSamplerParameterfv),ENTRY(glSamplerParameterIiv),ENTRY(glSamplerParameterIuiv),ENTRY(glDeleteSamplers),ENTRY(glCompressedTexImage2D),ENTRY(glTexStorage3D),ENTRY(glTextureStorage2D),ENTRY(glTextureView),ENTRY(glPopAttrib),ENTRY(glCallList),ENTRY(glCallLists)
#undef HOOK_ORIGINAL
#define RESIDENT_ENTRIES EPOCH_ENTRIES,ENTRY(glClear),ENTRY(glClearBufferfv),ENTRY(glClearBufferiv),ENTRY(glClearBufferuiv),ENTRY(glClearBufferfi),ENTRY(glReadPixels),ENTRY(glReadnPixels),ENTRY(glGetTexImage),ENTRY(glGetTextureImage),ENTRY(glBlitFramebuffer),ENTRY(glCopyTexSubImage2D),ENTRY(glCopyTexImage2D),ENTRY(glCopyImageSubData),ENTRY(glTexSubImage2D),ENTRY(glTexImage2D),ENTRY(glTexStorage2D),ENTRY(glTextureSubImage2D),ENTRY(glGenerateMipmap),ENTRY(glGenerateTextureMipmap),ENTRY(glClearTexImage),ENTRY(glClearTexSubImage),ENTRY(glDeleteTextures),ENTRY(glFlush),ENTRY(glFinish),ENTRY(glFenceSync),ENTRY(glXSwapBuffers)

static void resident_report(struct bridge_context *c){
 struct resident_state *s=c->resident;if(!s)return;
 note("resident_summary context_id=%llu draws=%llu gpu_clears=%llu uploads=%llu writebacks=%llu syncs=%llu",(unsigned long long)c->id,(unsigned long long)s->draws,(unsigned long long)s->gpu_clears,(unsigned long long)s->uploads,(unsigned long long)s->writebacks,(unsigned long long)s->syncs);
}

static int resident_is_bound(struct bridge_context *c){return c->resident&&c->resident->bound;}
// Around every Mesa draw in a resident context: Mesa must see current data
// (targets and anything it samples), and afterwards owns its framebuffer.
// ---- region-bounded Mesa draws ---------------------------------------------
// A Mesa draw can only touch pixels inside its viewport (plus line/point
// width) and scissor box. When that rectangle is small, only it crosses
// between Metal and Mesa: written back before the draw where Mesa is behind,
// read back into Metal after. Metal stays authoritative everywhere else, so
// the next GPU draw needs no whole-target upload.
struct region_plan {int active,x,y,w,h;unsigned n;struct resident *record[10];int mesa_was_current[10];GLuint names[10];};
static LR_TLS struct region_plan region_plan;
static int resident_region_transfer(struct bridge_context *c,struct resident *r,int x,int y,int w,int h,int to_mesa){
 size_t bytes=(size_t)w*h*lr_gpu_resident_pixel_bytes(r->format);void *pixels=malloc(bytes?bytes:1);if(!pixels)return 0;uint64_t started=report_now();
 GLenum format=r->part==RESIDENT_DEPTH?GL_DEPTH_COMPONENT:transfer_format(r),type=r->part==RESIDENT_DEPTH?GL_FLOAT:r->type;int ok=1;
 struct lr_gpu_resident q={.command=to_mesa?LR_GPU_RES_READ:LR_GPU_RES_UPLOAD,.id=r->id,.flags=LR_GPU_RES_FLAG_REGION,.x=(unsigned)x,.y=(unsigned)y,.width=(unsigned)w,.height=(unsigned)h};
 if(to_mesa){
  if(!resident_call(c,&q,pixels,bytes)){free(pixels);return 0;}
  struct pixel_state unpack;pixel_begin(&unpack,0);
  if(!r->name){   // window: blit the rectangle from a transfer texture into framebuffer 0's back buffer
   GLint read_fb,draw_fb,texture;glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING,&read_fb);glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING,&draw_fb);glGetIntegerv(GL_TEXTURE_BINDING_2D,&texture);
   GLboolean scissor=glIsEnabled(GL_SCISSOR_TEST),srgb=glIsEnabled(GL_FRAMEBUFFER_SRGB),discard=glIsEnabled(GL_RASTERIZER_DISCARD);
   if(!c->transfer_texture)glGenTextures(1,&c->transfer_texture);if(!c->transfer_fbo)glGenFramebuffers(1,&c->transfer_fbo);
   glBindTexture(GL_TEXTURE_2D,c->transfer_texture);glTexImage2D(GL_TEXTURE_2D,0,GL_RGBA8,w,h,0,GL_RGBA,GL_UNSIGNED_BYTE,pixels);
   glBindFramebuffer(GL_READ_FRAMEBUFFER,c->transfer_fbo);glFramebufferTexture2D(GL_READ_FRAMEBUFFER,GL_COLOR_ATTACHMENT0,GL_TEXTURE_2D,c->transfer_texture,0);glReadBuffer(GL_COLOR_ATTACHMENT0);
   glBindFramebuffer(GL_DRAW_FRAMEBUFFER,0);GLint window_buffer=GL_BACK;glGetIntegerv(GL_DRAW_BUFFER,&window_buffer);glDrawBuffer(GL_BACK);
   if(scissor)glDisable(GL_SCISSOR_TEST);if(srgb)glDisable(GL_FRAMEBUFFER_SRGB);if(discard)glDisable(GL_RASTERIZER_DISCARD);
   glBlitFramebuffer(0,0,w,h,x,y,x+w,y+h,GL_COLOR_BUFFER_BIT,GL_NEAREST);
   if(scissor)glEnable(GL_SCISSOR_TEST);if(srgb)glEnable(GL_FRAMEBUFFER_SRGB);if(discard)glEnable(GL_RASTERIZER_DISCARD);
   glDrawBuffer(window_buffer);glBindFramebuffer(GL_DRAW_FRAMEBUFFER,draw_fb);glBindFramebuffer(GL_READ_FRAMEBUFFER,read_fb);glBindTexture(GL_TEXTURE_2D,texture);
  }else{GLint prior;glGetIntegerv(GL_TEXTURE_BINDING_2D,&prior);glBindTexture(GL_TEXTURE_2D,r->name);
   typeof(&glTexSubImage2D) sub=original("glTexSubImage2D");if(sub)sub(GL_TEXTURE_2D,0,x,y,w,h,format,type,pixels);else ok=0;glBindTexture(GL_TEXTURE_2D,prior);}
  pixel_end(&unpack,0);
  if(ok)report_add(REPORT_WRITEBACK,started,bytes);
 }else{
  struct pixel_state pack;pixel_begin(&pack,1);
  if(!r->name){GLint read_fb;glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING,&read_fb);glBindFramebuffer(GL_READ_FRAMEBUFFER,0);GLint window_read=GL_BACK;glGetIntegerv(GL_READ_BUFFER,&window_read);
   glReadBuffer(GL_BACK);glReadPixels(x,y,w,h,GL_RGBA,GL_UNSIGNED_BYTE,pixels);glReadBuffer(window_read);glBindFramebuffer(GL_READ_FRAMEBUFFER,read_fb);}
  else glGetTextureSubImage(r->name,0,x,y,0,w,h,1,format,type,(GLsizei)bytes,pixels);
  pixel_end(&pack,1);
  ok=resident_call(c,&q,pixels,bytes);if(ok)report_add(REPORT_UPLOAD,started,bytes);
 }
 free(pixels);return ok;
}
// Plan a region-bounded Mesa draw; 0 means use the whole-target path.
static int resident_plan_region(struct bridge_context *c,GLenum mode){
 struct region_plan *plan=&region_plan;memset(plan,0,sizeof *plan);struct resident_state *s=c->resident;if(!s)return 0;
 if(glIsEnabled(GL_STENCIL_TEST)||glIsEnabled(GL_RASTERIZER_DISCARD)){report_reasonf("rp:stencil-or-discard");return 0;}
 if(mode==GL_POINTS&&glIsEnabled(GL_PROGRAM_POINT_SIZE)){report_reasonf("rp:program-point-size");return 0;}   // shader-chosen point size: unbounded
 GLint fb=0;glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING,&fb);unsigned W=0,H=0;
 if(!fb){window_size(&W,&H);}
 GLint viewport[4];glGetIntegerv(GL_VIEWPORT,viewport);GLfloat line=1,point=1;glGetFloatv(GL_LINE_WIDTH,&line);glGetFloatv(GL_POINT_SIZE,&point);
 int margin=(int)ceilf(fmaxf(line,point))+2;
 long x0=(long)viewport[0]-margin,y0=(long)viewport[1]-margin,x1=(long)viewport[0]+viewport[2]+margin,y1=(long)viewport[1]+viewport[3]+margin;
 if(glIsEnabled(GL_SCISSOR_TEST)){GLint box[4];glGetIntegerv(GL_SCISSOR_BOX,box);if(box[0]>x0)x0=box[0];if(box[1]>y0)y0=box[1];if((long)box[0]+box[2]<x1)x1=(long)box[0]+box[2];if((long)box[1]+box[3]<y1)y1=(long)box[1]+box[3];}
 // Attachments the draw can write, with their records in this context.
 GLuint names[10];enum resident_part parts[10];unsigned n=0;
 if(!fb&&glIsEnabled(GL_DEPTH_TEST)){report_reasonf("rp:window-depth");return 0;}   // whole sync covers the window depth record
 if(!fb){names[n]=0;parts[n++]=RESIDENT_COLOR;}
 else{for(unsigned i=0;i<8;i++){GLint buffer=GL_NONE;glGetIntegerv(GL_DRAW_BUFFER0+i,&buffer);if(buffer==GL_NONE)continue;if(buffer<GL_COLOR_ATTACHMENT0||buffer>GL_COLOR_ATTACHMENT15)return 0;
   GLint kind=0,object=0,level=0;glGetFramebufferAttachmentParameteriv(GL_DRAW_FRAMEBUFFER,buffer,GL_FRAMEBUFFER_ATTACHMENT_OBJECT_TYPE,&kind);if(kind==GL_NONE)continue;if(kind!=GL_TEXTURE)return 0;
   glGetFramebufferAttachmentParameteriv(GL_DRAW_FRAMEBUFFER,buffer,GL_FRAMEBUFFER_ATTACHMENT_OBJECT_NAME,&object);glGetFramebufferAttachmentParameteriv(GL_DRAW_FRAMEBUFFER,buffer,GL_FRAMEBUFFER_ATTACHMENT_TEXTURE_LEVEL,&level);if(level)return 0;
   names[n]=(GLuint)object;parts[n++]=RESIDENT_COLOR;}
  if(glIsEnabled(GL_DEPTH_TEST)){GLint kind=0,object=0;glGetFramebufferAttachmentParameteriv(GL_DRAW_FRAMEBUFFER,GL_DEPTH_ATTACHMENT,GL_FRAMEBUFFER_ATTACHMENT_OBJECT_TYPE,&kind);
   if(kind==GL_TEXTURE){glGetFramebufferAttachmentParameteriv(GL_DRAW_FRAMEBUFFER,GL_DEPTH_ATTACHMENT,GL_FRAMEBUFFER_ATTACHMENT_OBJECT_NAME,&object);names[n]=(GLuint)object;parts[n++]=RESIDENT_DEPTH;}else if(kind!=GL_NONE)return 0;}}
 for(unsigned i=0;i<n;i++){struct resident *r=resident_find(s,names[i],parts[i]);
  if(r&&r->part==RESIDENT_DEPTH&&packed_depth_stencil(r->internal)){report_reasonf("rp:packed-depth");return 0;}   // stencil half would need the same treatment
  if(r){W=W?(W<r->width?W:r->width):r->width;H=H?(H<r->height?H:r->height):r->height;}}
 if(!W||!H){report_reasonf("rp:no-size");return 0;}
 if(x0<0)x0=0;if(y0<0)y0=0;if(x1>(long)W)x1=W;if(y1>(long)H)y1=H;
 if(x1<=x0||y1<=y0){plan->active=1;return 1;}   // the draw cannot touch any pixel
 if((x1-x0)*(y1-y0)*10>(long)W*H*6){report_reasonf("rp:large-mode%x-%ldx%ld",mode,x1-x0,y1-y0);return 0;}      // most of the target: the whole path is as cheap
 plan->x=(int)x0;plan->y=(int)y0;plan->w=(int)(x1-x0);plan->h=(int)(y1-y0);
 for(unsigned i=0;i<n;i++){struct resident *r=resident_find(s,names[i],parts[i]);if(!r)continue;
  pthread_mutex_lock(&generation_lock);struct texture_generation *g=generation_entry(resident_group(c,r->name),r->name,0);
  int metal=g&&r->metal_gen==g->gen,mesa=g&&g->mesa_gen==g->gen;pthread_mutex_unlock(&generation_lock);
  if(!metal)continue;   // Mesa holds the image; the draw just updates it there
  plan->record[plan->n]=r;plan->names[plan->n]=r->name;plan->mesa_was_current[plan->n++]=mesa;}
 plan->active=1;return 1;
}
// ---- what a Mesa draw can read ---------------------------------------------
// Sampler uniform locations per linked program, keyed by its capture
// generation (a relink makes a new one). Enumerating uniforms on every Mesa
// draw cost more than it saved; cached, a fallback draw only syncs its own
// framebuffer attachments and the 2D textures its samplers name.
#define SAMPLER_CACHE 64
struct sampler_cache {uintptr_t group;GLuint program;uint64_t generation;int images;unsigned count;GLint location[24];};
static struct sampler_cache sampler_cache[SAMPLER_CACHE];static unsigned sampler_cache_next;
static pthread_mutex_t sampler_cache_lock=PTHREAD_MUTEX_INITIALIZER;
static int sampler_2d_type(GLenum type){return type==GL_SAMPLER_2D||type==GL_SAMPLER_2D_SHADOW||type==GL_INT_SAMPLER_2D||type==GL_UNSIGNED_INT_SAMPLER_2D;}
static int program_samplers(struct bridge_context *c,GLuint program,struct sampler_cache *out){
 uint64_t generation=0;pthread_mutex_lock(&records);for(unsigned i=0;i<CAPTURES;i++)if(captures[i].group==c->group&&captures[i].name==program){generation=captures[i].generation;break;}pthread_mutex_unlock(&records);
 if(!generation)return 0;
 pthread_mutex_lock(&sampler_cache_lock);
 for(unsigned i=0;i<SAMPLER_CACHE;i++)if(sampler_cache[i].group==c->group&&sampler_cache[i].program==program&&sampler_cache[i].generation==generation){*out=sampler_cache[i];pthread_mutex_unlock(&sampler_cache_lock);return 1;}
 pthread_mutex_unlock(&sampler_cache_lock);
 struct sampler_cache e={.group=c->group,.program=program,.generation=generation};GLint count=0;glGetProgramiv(program,GL_ACTIVE_UNIFORMS,&count);
 for(GLint u=0;u<count;u++){char name[256];GLint size=0;GLenum type=0;glGetActiveUniform(program,u,sizeof name,NULL,&size,&type,name);
  if(type>=GL_IMAGE_1D&&type<=GL_UNSIGNED_INT_IMAGE_2D_MULTISAMPLE_ARRAY)e.images=1;
  if(!sampler_2d_type(type))continue;char *bracket=strchr(name,'[');if(bracket)*bracket=0;
  for(GLint k=0;k<size;k++){char element[300];if(size>1)snprintf(element,sizeof element,"%s[%d]",name,k);else snprintf(element,sizeof element,"%s",name);
   GLint location=glGetUniformLocation(program,element);if(location<0)continue;if(e.count>=24)return 0;e.location[e.count++]=location;}}
 pthread_mutex_lock(&sampler_cache_lock);sampler_cache[sampler_cache_next++%SAMPLER_CACHE]=e;pthread_mutex_unlock(&sampler_cache_lock);
 *out=e;return 1;
}
// Before a Mesa draw: its draw-framebuffer attachments and sampled 2D textures.
static void resident_sync_for_mesa_draw(struct bridge_context *c){
 GLenum pending=resident_errors_begin();resident_reentry++;
 GLuint names[40];unsigned n=0;int all=0;GLint fb=0,program=0,pipeline=0;
 glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING,&fb);glGetIntegerv(GL_CURRENT_PROGRAM,&program);glGetIntegerv(GL_PROGRAM_PIPELINE_BINDING,&pipeline);
 struct sampler_cache sc;
 if(!program||pipeline||!program_samplers(c,(GLuint)program,&sc)||sc.images)all=1;
 else{
  if(!fb){names[n++]=0;if(mesa_draw_reads_depth())names[n++]=RESIDENT_WINDOW_DEPTH;}
  else{GLenum points[]={GL_COLOR_ATTACHMENT0,GL_COLOR_ATTACHMENT1,GL_COLOR_ATTACHMENT2,GL_COLOR_ATTACHMENT3,GL_COLOR_ATTACHMENT4,GL_COLOR_ATTACHMENT5,GL_COLOR_ATTACHMENT6,GL_COLOR_ATTACHMENT7,GL_DEPTH_ATTACHMENT,GL_STENCIL_ATTACHMENT};
   int depth=mesa_draw_reads_depth(),stencil=mesa_draw_uses_stencil();
   for(unsigned i=0;i<10;i++){if((points[i]==GL_DEPTH_ATTACHMENT&&!depth)||(points[i]==GL_STENCIL_ATTACHMENT&&!stencil))continue;   // untested buffers are neither read nor written
    GLint kind=0,object=0;glGetFramebufferAttachmentParameteriv(GL_DRAW_FRAMEBUFFER,points[i],GL_FRAMEBUFFER_ATTACHMENT_OBJECT_TYPE,&kind);
    if(kind==GL_TEXTURE){glGetFramebufferAttachmentParameteriv(GL_DRAW_FRAMEBUFFER,points[i],GL_FRAMEBUFFER_ATTACHMENT_OBJECT_NAME,&object);names[n++]=(GLuint)object;}}}
  GLint prior=0;glGetIntegerv(GL_ACTIVE_TEXTURE,&prior);
  for(unsigned i=0;i<sc.count;i++){GLint unit=-1,texture=0;glGetUniformiv(program,sc.location[i],&unit);if(unit<0)continue;
   glActiveTexture(GL_TEXTURE0+unit);glGetIntegerv(GL_TEXTURE_BINDING_2D,&texture);if(texture&&n<40)names[n++]=(GLuint)texture;}
  glActiveTexture(prior);}
 resident_reentry--;resident_errors_end(c,pending);
 if(all){report_reasonf("sync:mesa-draw-all");resident_sync(c,0,0);return;}
 for(unsigned i=0;i<n;i++)resident_sync(c,names[i],1);
}
static int draws_to_fbo(struct bridge_context *c){(void)c;GLint fb=0;glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING,&fb);return fb!=0;}
static void resident_around_mesa_draw(int after,GLenum mode){
 if(!resident_enabled())return;struct bridge_context *c=current_context();
 if(!c||c->owner!=current_tid())return;
 static LR_TLS uint64_t started;
 if(!after){
  if(c->resident)c->resident->mesa_draws++;
  started=report_now();GLenum pending=resident_errors_begin();resident_reentry++;
  int region=resident_plan_region(c,mode);struct region_plan *plan=&region_plan;
  if(region){
   // Everything the draw may sample stays whole-synced; its own targets go by rectangle.
   struct resident_state *s=c->resident;GLint program=0,pipeline=0;glGetIntegerv(GL_CURRENT_PROGRAM,&program);glGetIntegerv(GL_PROGRAM_PIPELINE_BINDING,&pipeline);
   struct sampler_cache sc;
   if(program&&!pipeline&&program_samplers(c,(GLuint)program,&sc)&&!sc.images){   // only the textures its samplers name
    GLint prior=0;glGetIntegerv(GL_ACTIVE_TEXTURE,&prior);GLuint sampled[24];unsigned m=0;
    for(unsigned i=0;i<sc.count;i++){GLint unit=-1,texture=0;glGetUniformiv(program,sc.location[i],&unit);if(unit<0)continue;glActiveTexture(GL_TEXTURE0+unit);glGetIntegerv(GL_TEXTURE_BINDING_2D,&texture);if(texture)sampled[m++]=(GLuint)texture;}
    glActiveTexture(prior);
    for(unsigned i=0;i<m;i++){int mine=0;for(unsigned k=0;k<plan->n;k++)if(plan->names[k]==sampled[i])mine=1;if(!mine){resident_reentry--;resident_sync(c,sampled[i],1);resident_reentry++;}}
   }else
   for(unsigned i=0;i<s->limit;i++){struct resident *r=&s->record[i];if(!r->in_use)continue;int mine=0;for(unsigned k=0;k<plan->n;k++)if(plan->record[k]==r)mine=1;
    if(!mine&&(r->name||!draws_to_fbo(c))){resident_reentry--;resident_sync(c,r->name,1);resident_reentry++;}}   // the window cannot be sampled
   for(unsigned k=0;k<plan->n;k++)if(!plan->mesa_was_current[k]&&plan->w&&!resident_region_transfer(c,plan->record[k],plan->x,plan->y,plan->w,plan->h,1)){
    // Could not stage the rectangle: fall back to the whole sync for safety.
    report_reasonf("sync:region-failed");plan->active=0;resident_reentry--;resident_sync(c,0,0);resident_reentry++;break;}
   if(plan->active)report_reasonf("sync:mesa-draw-region");
  }
  resident_reentry--;resident_errors_end(c,pending);
  if(!region){report_reasonf("sync:mesa-draw");resident_sync_for_mesa_draw(c);}
  return;
 }
 struct region_plan *plan=&region_plan;
 if(plan->active){
  GLenum pending=resident_errors_begin();resident_reentry++;
  for(unsigned k=0;k<plan->n;k++){struct resident *r=plan->record[k];int ok=plan->w?resident_region_transfer(c,r,plan->x,plan->y,plan->w,plan->h,0):1;
   pthread_mutex_lock(&generation_lock);struct texture_generation *g=generation_entry(resident_group(c,r->name),r->name,1);
   if(g&&plan->mesa_was_current[k]){g->gen=++generation_clock;g->mesa_gen=g->gen;if(ok)r->metal_gen=g->gen;}   // Mesa complete and current; Metal too if the rectangle arrived
   else if(!ok)note("resident_region_upload_failed name=%u",r->name);   // Metal stays authoritative; the rectangle in it is stale
   pthread_mutex_unlock(&generation_lock);}
  // Attachments without a current Metal copy were updated in Mesa only.
  resident_mesa_draw_wrote(c,plan->names,plan->n);
  plan->active=0;resident_reentry--;resident_errors_end(c,pending);report_add(REPORT_MESA_DRAW,started,0);return;
 }
 resident_mesa_draw_wrote(c,NULL,0);report_add(REPORT_MESA_DRAW,started,0);
}
