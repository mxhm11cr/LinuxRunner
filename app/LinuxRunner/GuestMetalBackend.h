// Private implementation included once by RuntimeBridge (also the host driver).
// Offscreen guest rendering, separate from the final framebuffer presenter.
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <TargetConditionals.h>
#include "graphics/guest_gpu.h"
#include <stdatomic.h>
#if defined(__aarch64__)
#include <arm_neon.h>
#endif
@interface LRGuestMetalProgram : NSObject
@property(nonatomic,strong) id<MTLRenderPipelineState> pipeline;
@property(nonatomic) struct lr_gl_link reflection;
@property(nonatomic,strong) NSData *blocksData;
@property(nonatomic) struct lr_gl_uniform_extents extents;
@property(nonatomic) struct lr_gl_storages storages;
@property(nonatomic,strong) NSMutableDictionary<NSNumber *,id<MTLBuffer>> *storageBuffers;
@property(nonatomic,strong) NSMutableDictionary<NSNumber *,NSMutableData *> *arrayData;
@property(nonatomic,strong) MTLRenderPipelineDescriptor *descriptor;
@end
@implementation LRGuestMetalProgram
@end
@interface LRGuestMetalTexture : NSObject
@property(nonatomic,strong) id<MTLTexture> texture,renderTexture;
@property(nonatomic) BOOL painted;
@property(nonatomic,strong) id<MTLSamplerState> sampler;
@property(nonatomic) unsigned minFilter,magFilter,wrapS,wrapT,colorFormat,storagePixelBytes;
@property(nonatomic) MTLPixelFormat storagePixelFormat;
@property(nonatomic) BOOL mipmaps;
@property(nonatomic) NSUInteger bytes;
@end
@implementation LRGuestMetalTexture
@end
@interface LRGuestMetalRenderbuffer : NSObject
@property(nonatomic,strong) id<MTLTexture> texture;
@property(nonatomic) unsigned width,height,format;
@end
@implementation LRGuestMetalRenderbuffer
@end
@interface LRGuestMetalFramebuffer : NSObject
@property(nonatomic,strong) LRGuestMetalTexture *color;
@property(nonatomic,strong) LRGuestMetalRenderbuffer *depth;
@property(nonatomic) unsigned depthName;
@property(nonatomic) unsigned textureName,format;
@end
@implementation LRGuestMetalFramebuffer
@end
static unsigned LRTexturePixelBytes(unsigned format){return lr_gpu_texture_pixel_bytes(format);}
static MTLPixelFormat LRTextureNativeFormat(unsigned format){switch(format){case LR_GPU_TEXTURE_FORMAT_DEPTH_COMPONENT:return MTLPixelFormatR32Float;case LR_GPU_TEXTURE_FORMAT_RGBA16F:return MTLPixelFormatRGBA16Float;case LR_GPU_TEXTURE_FORMAT_SRGB8_ALPHA8:return MTLPixelFormatRGBA8Unorm_sRGB;case LR_GPU_TEXTURE_FORMAT_R8:case LR_GPU_TEXTURE_FORMAT_RG8:case LR_GPU_TEXTURE_FORMAT_RGB8:case LR_GPU_TEXTURE_FORMAT_RGBA8:case LR_GPU_TEXTURE_FORMAT_ALPHA8:case LR_GPU_TEXTURE_FORMAT_LUMINANCE8:case LR_GPU_TEXTURE_FORMAT_LUMINANCE_ALPHA8:return MTLPixelFormatRGBA8Unorm;default:return MTLPixelFormatInvalid;}}
static NSUInteger LRTextureBytes(unsigned w,unsigned h,unsigned format){NSUInteger bytes=0,pixel=LRTexturePixelBytes(format);for(;;){bytes+=(NSUInteger)w*h*pixel;if(w==1&&h==1)return bytes;w=w>1?w/2:1;h=h>1?h/2:1;}}
@interface LRGuestMetalContext : NSObject
@property(nonatomic,strong) id<MTLDevice> device;
@property(nonatomic,strong) id<MTLCommandQueue> queue;
@property(nonatomic,strong) id<MTLComputePipelineState> flipPipeline;
@property(nonatomic) uint64_t computeCopies,rowCopies,copyBytes;
@property(nonatomic,strong) NSMutableData *instanceData;
@property(nonatomic,strong) NSMutableDictionary<NSString *,NSArray *> *instancePipelines;
@property(nonatomic,strong) id<MTLRenderPipelineState> pipeline;
@property(nonatomic,strong) MTLRenderPipelineDescriptor *descriptor;
@property(nonatomic) struct lr_gpu_blend blendState;
@property(nonatomic) struct lr_gpu_raster_state rasterState;
@property(nonatomic) struct lr_gpu_stencil_state stencilState;
@property(nonatomic,strong) NSMutableDictionary<NSString *,NSArray *> *blendPipelines;
@property(nonatomic,strong) id<MTLTexture> target, depth,stencil,defaultTarget,defaultDepth,defaultStencil,offscreenDepth,offscreenStencil,defaultSrgbView;
@property(nonatomic) BOOL defaultPainted, defaultStencilPainted, offscreenStencilPainted;
@property(nonatomic,strong) NSArray<id<MTLTexture>> *mrtColors;
@property(nonatomic,strong) NSData *mrtMasks;
@property(nonatomic) unsigned boundFramebuffer;
@property(nonatomic,strong) NSMutableDictionary<NSNumber *,LRGuestMetalRenderbuffer *> *renderbuffers;
@property(nonatomic,strong) NSMutableDictionary<NSNumber *,LRGuestMetalFramebuffer *> *framebuffers;
@property(nonatomic,strong) id<MTLDepthStencilState> depthOn, depthOff;
@property(nonatomic) struct lr_gpu_depth_state depthState;
@property(nonatomic,strong) NSMutableDictionary<NSData *,id<MTLDepthStencilState>> *depthStates;

@property(nonatomic) uint64_t submissions, readbacks, gpuNanoseconds;
@property(nonatomic) BOOL painted;
@property(nonatomic,strong) id<MTLCommandBuffer> pending;
@property(nonatomic,strong) id<MTLRenderCommandEncoder> encoder;
@property(nonatomic) unsigned pendingDraws;
@property(nonatomic) NSUInteger pendingBytes;
@property(nonatomic,strong) NSMutableDictionary<NSNumber *,NSValue *> *shaders;
@property(nonatomic,strong) NSMutableDictionary<NSNumber *,LRGuestMetalProgram *> *programs;
@property(nonatomic,strong) NSMutableDictionary<NSString *,LRGuestMetalProgram *> *pipelineCache;
@property(nonatomic,strong) NSMutableDictionary<NSNumber *,NSArray<NSMutableData *> *> *uniformBlockData;
@property(nonatomic,strong) NSMutableDictionary<NSNumber *,LRGuestMetalTexture *> *textures;
@property(nonatomic,strong) id<MTLTexture> incompleteTexture;
@property(nonatomic,strong) id<MTLSamplerState> incompleteSampler;
@property(nonatomic,strong) id<MTLBuffer> vertexArena,depthTransfer;
// Resident mode submits full command buffers without waiting (LRGuestMetalSubmit):
// arenas rotate through a ring, each reused only after the command buffer that
// read it completed. Finish still waits for everything (CPU access follows it).
@property(nonatomic,strong) NSMutableArray *arenaRing,*arenaFences;
@property(nonatomic,strong) id<MTLCommandBuffer> lastSubmitted;
@property(nonatomic) NSUInteger arenaIndex;
@property(nonatomic) uint64_t asyncSubmissions,arenaWaits;
@property(nonatomic) NSUInteger vertexOffset,textureBytes;
@property(nonatomic) struct lr_gpu_rectangle viewport;
@property(nonatomic) uint64_t blockUploadCalls,blockUploadBytes,blockSnapshotBytes;
@property(nonatomic) uint64_t vertexAllocations,vertexUploadBytes,textureAllocations,textureUploadBytes;
// GPU-resident textures for the Mesa bridge (LR_GPU_RESIDENT; GuestMetalResident.h).
@property(nonatomic,strong) NSMutableDictionary<NSNumber *,id> *residents;
@property(nonatomic) BOOL residentBound;
@property(nonatomic) NSUInteger residentBytes;
@property(nonatomic,strong) NSArray<id<MTLTexture>> *companionMrt;
@property(nonatomic,strong) NSData *companionMasks;
@property(nonatomic,strong) id<MTLTexture> residentScratchDepth,residentScratchStencil;
@property(nonatomic) BOOL residentSrgb; // bound resident colour targets encode sRGB
@property(nonatomic,strong) id<MTLLibrary> residentClearLibrary,residentBlitLibrary;
@property(nonatomic,strong) NSMutableDictionary<NSString *,id> *residentClearStates;
@property(nonatomic) uint64_t residentUploads,residentReads,residentClears,residentFlips,residentBinds,residentBlits;
@end
@implementation LRGuestMetalContext
- (void)dealloc { for(NSValue *v in self.shaders.allValues) lr_glsl_shader_destroy(v.pointerValue); }
@end
static int LRGuestSelectTarget(LRGuestMetalContext *c);
// Leave resident render targets: close the open encoder (the command buffer
// stays pending) and restore the companion layout the bridge's per-draw path
// uses. Every companion import/read path calls this first.
static int LRGuestResidentRelease(LRGuestMetalContext *c){
 if(!c.residentBound)return 0;
 if(c.encoder){[c.encoder endEncoding];c.encoder=nil;}
 c.mrtColors=c.companionMrt;c.mrtMasks=c.companionMasks;c.companionMrt=nil;c.companionMasks=nil;c.residentBound=NO;c.residentSrgb=NO;
 return LRGuestSelectTarget(c);
}
static void *LRGuestMetalCreate(unsigned width,unsigned height) {
    @autoreleasepool {
        LRGuestMetalContext *c=[LRGuestMetalContext new];
        c.device=MTLCreateSystemDefaultDevice();if(!c.device)return NULL;
        c.renderbuffers=[NSMutableDictionary new];c.framebuffers=[NSMutableDictionary new];c.textures=[NSMutableDictionary new];c.shaders=[NSMutableDictionary new];c.programs=[NSMutableDictionary new];c.pipelineCache=[NSMutableDictionary new];
        c.rasterState=(struct lr_gpu_raster_state){.color_mask=15,.cull_face=0x0405,.front_face=0x0901};c.depthStates=[NSMutableDictionary new];c.depthState=(struct lr_gpu_depth_state){.function=0x0201,.write_mask=1,.clear_depth=1};
        c.blendPipelines=[NSMutableDictionary new];c.blendState=(struct lr_gpu_blend){.src_rgb=1,.src_alpha=1,.equation_rgb=0x8006,.equation_alpha=0x8006};c.uniformBlockData=[NSMutableDictionary new];c.queue=[c.device newCommandQueue];c.viewport=(struct lr_gpu_rectangle){0,0,width,height};
        // Fixed-function GLES subset: guest-provided clip coordinates/colors,
        // real GPU interpolation/rasterization. GLES2 uses separate compiled programs.
        NSString *source=@"#include <metal_stdlib>\nusing namespace metal;\nstruct V { float4 p; float4 c; }; struct O {float4 p [[position]];float4 c;}; vertex O lr_guest_v(device const V *v [[buffer(0)]],uint n [[vertex_id]]) {O o;o.p=v[n].p;o.p.z=(o.p.z+o.p.w)*0.5;o.c=v[n].c;return o;} fragment float4 lr_guest_f(O o [[stage_in]]) {return o.c;} kernel void lr_flip(texture2d<float,access::read> source [[texture(0)]],texture2d<float,access::write> dest [[texture(1)]],uint2 p [[thread_position_in_grid]]) {if(p.x<source.get_width()&&p.y<source.get_height())dest.write(source.read(p),uint2(p.x,source.get_height()-1-p.y));}";
        NSError *error=nil;id<MTLLibrary> library=[c.device newLibraryWithSource:source options:nil error:&error];
        if(!library)return NULL;
        c.flipPipeline=[c.device newComputePipelineStateWithFunction:[library newFunctionWithName:@"lr_flip"] error:&error];
        if(!c.flipPipeline)fprintf(stderr,"GUEST_METAL_FBO_COPY compute unavailable; bounded row-blit fallback: %s\n",error.localizedDescription.UTF8String);
        MTLRenderPipelineDescriptor *pipeline=[MTLRenderPipelineDescriptor new];
        pipeline.vertexFunction=[library newFunctionWithName:@"lr_guest_v"];
        pipeline.fragmentFunction=[library newFunctionWithName:@"lr_guest_f"];
        pipeline.colorAttachments[0].pixelFormat=MTLPixelFormatRGBA8Unorm;
        pipeline.depthAttachmentPixelFormat=MTLPixelFormatDepth32Float;pipeline.stencilAttachmentPixelFormat=MTLPixelFormatStencil8;
        c.descriptor=pipeline;c.pipeline=[c.device newRenderPipelineStateWithDescriptor:pipeline error:&error];
        MTLTextureDescriptor *t=[MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm width:width height:height mipmapped:NO];
        t.storageMode=MTLStorageModeShared;t.usage=MTLTextureUsageRenderTarget|MTLTextureUsagePixelFormatView;
        c.target=[c.device newTextureWithDescriptor:t];
        MTLTextureDescriptor *black=[MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm width:1 height:1 mipmapped:NO];black.storageMode=MTLStorageModeShared;black.usage=MTLTextureUsageShaderRead;
        c.incompleteTexture=[c.device newTextureWithDescriptor:black];uint8_t rgba[4]={0,0,0,255};[c.incompleteTexture replaceRegion:MTLRegionMake2D(0,0,1,1) mipmapLevel:0 withBytes:rgba bytesPerRow:4];
        MTLSamplerDescriptor *nearest=[MTLSamplerDescriptor new];nearest.minFilter=nearest.magFilter=MTLSamplerMinMagFilterNearest;c.incompleteSampler=[c.device newSamplerStateWithDescriptor:nearest];
        t.pixelFormat=MTLPixelFormatDepth32Float;t.storageMode=MTLStorageModePrivate;t.usage=MTLTextureUsageRenderTarget;
        c.depth=[c.device newTextureWithDescriptor:t];
        MTLTextureDescriptor *st=[MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatStencil8 width:width height:height mipmapped:NO];st.storageMode=MTLStorageModePrivate;st.usage=MTLTextureUsageRenderTarget;c.stencil=[c.device newTextureWithDescriptor:st];
        MTLDepthStencilDescriptor *ds=[MTLDepthStencilDescriptor new];
        ds.depthCompareFunction=MTLCompareFunctionLess;ds.depthWriteEnabled=YES;
        c.depthOn=[c.device newDepthStencilStateWithDescriptor:ds];
        ds.depthCompareFunction=MTLCompareFunctionAlways;ds.depthWriteEnabled=NO;
        c.depthOff=[c.device newDepthStencilStateWithDescriptor:ds];
        if(!c.pipeline||!c.queue||!c.target||!c.depth||!c.stencil||!c.depthOn||!c.depthOff||!c.incompleteTexture||!c.incompleteSampler)return NULL;
        c.defaultTarget=c.target;c.defaultDepth=c.depth;c.defaultStencil=c.stencil;
        return (__bridge_retained void *)c;
    }
}
// Count unique resources, including deleted textures retained by unbound FBOs.
static NSUInteger LRGuestTextureStorage(LRGuestMetalContext *c){
 NSMutableSet *images=[NSMutableSet setWithArray:c.textures.allValues];
 for(LRGuestMetalFramebuffer *f in c.framebuffers.allValues)if(f.color)[images addObject:f.color];
 NSUInteger bytes=0;for(LRGuestMetalTexture *t in images)bytes+=t.bytes+(t.renderTexture?t.renderTexture.width*t.renderTexture.height*4:0);return bytes;
}
static unsigned LRGuestFramebufferStatus(LRGuestMetalFramebuffer *f){
 if(!f.color)return f.depth?0x8cdd:0x8cd7; // INCOMPLETE_MISSING_ATTACHMENT
 if(!f.color.texture)return 0x8cd6; // INCOMPLETE_ATTACHMENT
 if(f.color.colorFormat!=0x1908)return 0x8cdd;
 if(f.depth&&!f.depth.texture)return 0x8cd6;
 if(f.depth&&(f.depth.width!=f.color.texture.width||f.depth.height!=f.color.texture.height))return 0x8cd9;
 return 0x8cd5;
}
static int LRGuestFlipTexture(LRGuestMetalContext *,id<MTLCommandBuffer>,id<MTLTexture>,id<MTLTexture>);
static int LRGuestSelectTarget(LRGuestMetalContext *c){
 // Resident targets stay selected until LRGuestResidentRelease; state updates
 // (raster, framebuffer bookkeeping) must not retarget the open pass.
 if(c.residentBound)return 0;
 if(!c.boundFramebuffer){c.target=c.defaultTarget;if(c.rasterState.framebuffer_srgb&&c.defaultTarget.pixelFormat==MTLPixelFormatRGBA8Unorm){if(!c.defaultSrgbView)c.defaultSrgbView=[c.defaultTarget newTextureViewWithPixelFormat:MTLPixelFormatRGBA8Unorm_sRGB];if(!c.defaultSrgbView)return -12;c.target=c.defaultSrgbView;}c.depth=c.defaultDepth;c.stencil=c.defaultStencil;c.painted=c.defaultPainted;return 0;}
 LRGuestMetalFramebuffer *framebuffer=c.framebuffers[@(c.boundFramebuffer)];LRGuestMetalTexture *t=framebuffer.color;
 if(!t.texture){c.target=nil;c.painted=NO;return 0;}
 if(!t.renderTexture){
  c.target=nil;c.painted=NO;
  NSUInteger bytes=t.texture.width*t.texture.height*4;
  if(bytes>LR_GPU_TEXTURE_BUDGET-LRGuestTextureStorage(c))return -12;
  MTLTextureDescriptor *d=[MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm width:t.texture.width height:t.texture.height mipmapped:NO];
  d.storageMode=MTLStorageModeShared;d.usage=MTLTextureUsageRenderTarget|MTLTextureUsageShaderRead|MTLTextureUsageShaderWrite;
  t.renderTexture=[c.device newTextureWithDescriptor:d];if(!t.renderTexture)return -12;
  id<MTLCommandBuffer> command=[c.queue commandBuffer];int copyError=LRGuestFlipTexture(c,command,t.texture,t.renderTexture);if(copyError)return copyError;[command commit];[command waitUntilCompleted];if(command.status!=MTLCommandBufferStatusCompleted)return -5;
 }
 if(c.offscreenDepth.width!=t.texture.width||c.offscreenDepth.height!=t.texture.height){
  MTLTextureDescriptor *d=[MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatDepth32Float width:t.texture.width height:t.texture.height mipmapped:NO];d.storageMode=MTLStorageModePrivate;d.usage=MTLTextureUsageRenderTarget|MTLTextureUsagePixelFormatView;
  id<MTLTexture> depth=[c.device newTextureWithDescriptor:d];if(!depth)return -12;c.offscreenDepth=depth;MTLTextureDescriptor *s=[MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatStencil8 width:t.texture.width height:t.texture.height mipmapped:NO];s.storageMode=MTLStorageModePrivate;s.usage=MTLTextureUsageRenderTarget;c.offscreenStencil=[c.device newTextureWithDescriptor:s];if(!c.offscreenStencil)return -12;c.offscreenStencilPainted=NO;
 }
 c.target=t.renderTexture;c.depth=framebuffer.depth?framebuffer.depth.texture:c.offscreenDepth;c.stencil=c.offscreenStencil;c.painted=YES; /* initialized from the attached texture */ return 0;
}
// GLES texture row zero is bottom; Metal targets use a top-left origin. GPU
// blits preserve orientation between render-target and sampled storage. No CPU
// readback is involved. Both storages are quota-accounted and reused.
static int LRGuestFlipTexture(LRGuestMetalContext *c,id<MTLCommandBuffer> command,id<MTLTexture> source,id<MTLTexture> dest){
 if(!source||!dest||source==dest||source.width!=dest.width||source.height!=dest.height)return -22;
 if(c.flipPipeline){
  id<MTLComputeCommandEncoder> e=[command computeCommandEncoder];if(!e)return -5;
  [e setComputePipelineState:c.flipPipeline];[e setTexture:source atIndex:0];[e setTexture:dest atIndex:1];
  NSUInteger rows=MIN((NSUInteger)16,c.flipPipeline.maxTotalThreadsPerThreadgroup/16);if(!rows){[e endEncoding];return -5;}
  [e dispatchThreadgroups:MTLSizeMake((source.width+15)/16,(source.height+rows-1)/rows,1) threadsPerThreadgroup:MTLSizeMake(16,rows,1)];[e endEncoding];c.computeCopies++;c.copyBytes+=(uint64_t)source.width*source.height*4;return 0;
 }
 c.rowCopies+=source.height;c.copyBytes+=(uint64_t)source.width*source.height*4;
 id<MTLBlitCommandEncoder> b=[command blitCommandEncoder];if(!b)return -5;
 for(NSUInteger y=0;y<source.height;y++)[b copyFromTexture:source sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0,y,0) sourceSize:MTLSizeMake(source.width,1,1) toTexture:dest destinationSlice:0 destinationLevel:0 destinationOrigin:MTLOriginMake(0,source.height-1-y,0)];
 [b endEncoding];return 0;
}
// Bound queued work; readback/finish/error reporting is a synchronization point.
// Small client arrays are copied by Metal's setVertexBytes, not overwritten
// inside an in-flight shared vertex buffer. The large-array arena is reset only
// after GPU completion, so encoded draws retain distinct immutable byte ranges.
static int LRGuestMetalCompleted(LRGuestMetalContext *c,id<MTLCommandBuffer> command){
    [command waitUntilCompleted];
    if(command.status!=MTLCommandBufferStatusCompleted){c.painted=NO;fprintf(stderr,"GUEST_METAL_ERROR status=%lu error_code=%ld\n",(unsigned long)command.status,(long)command.error.code);return -5;}
    if(command.GPUEndTime>=command.GPUStartTime)c.gpuNanoseconds+=(uint64_t)((command.GPUEndTime-command.GPUStartTime)*1e9);
    return 0;
}
static int LRGuestMetalFinish(LRGuestMetalContext *c) {
    if(c.lastSubmitted&&!c.pending){id<MTLCommandBuffer> last=c.lastSubmitted;c.lastSubmitted=nil;return LRGuestMetalCompleted(c,last);}
    c.lastSubmitted=nil;   // the queue runs in order: completing the newest buffer completes the earlier ones
    if(!c.pending)return 0;
    if(c.encoder){[c.encoder endEncoding];c.encoder=nil;}
    id<MTLCommandBuffer> command=c.pending;c.pending=nil;
    if(c.boundFramebuffer&&c.painted&&!c.residentBound){LRGuestMetalTexture *t=c.framebuffers[@(c.boundFramebuffer)].color;
      int copyError=LRGuestFlipTexture(c,command,c.target,t.texture);if(copyError){c.painted=NO;return copyError;}t.painted=YES;t.mipmaps=NO;
    }else if(!c.boundFramebuffer&&!c.residentBound)c.defaultPainted=c.painted;
    [command commit];[command waitUntilCompleted];
    unsigned draws=c.pendingDraws;c.pendingDraws=0;c.pendingBytes=0;c.vertexOffset=0;
    if(command.status!=MTLCommandBufferStatusCompleted){c.painted=NO;fprintf(stderr,"GUEST_METAL_ERROR status=%lu error_code=%ld\n",(unsigned long)command.status,(long)command.error.code);return -5;}
    c.submissions+=draws;
    if(command.GPUEndTime>=command.GPUStartTime)c.gpuNanoseconds+=(uint64_t)((command.GPUEndTime-command.GPUStartTime)*1e9);
    return 0;
}
// Batch boundary without a CPU wait (resident targets only: nothing is flipped or
// read back here). The next arena in the ring is reused once its reader completed.
static int LRGuestMetalSubmit(LRGuestMetalContext *c) {
    if(!c.pending)return 0;
    if(!c.residentBound||getenv("LR_GUEST_METAL_SYNC_SUBMIT"))return LRGuestMetalFinish(c);
    if(c.encoder){[c.encoder endEncoding];c.encoder=nil;}
    id<MTLCommandBuffer> command=c.pending;c.pending=nil;[command commit];
    c.lastSubmitted=command;c.submissions+=c.pendingDraws;c.asyncSubmissions++;c.pendingDraws=0;c.pendingBytes=0;
    enum{ring=4};
    if(!c.arenaRing){c.arenaRing=[NSMutableArray new];c.arenaFences=[NSMutableArray new];for(unsigned i=0;i<ring;i++){[c.arenaRing addObject:NSNull.null];[c.arenaFences addObject:NSNull.null];}}
    c.arenaRing[c.arenaIndex]=c.vertexArena?c.vertexArena:(id)NSNull.null;c.arenaFences[c.arenaIndex]=command;
    c.arenaIndex=(c.arenaIndex+1)%ring;
    id fence=c.arenaFences[c.arenaIndex],arena=c.arenaRing[c.arenaIndex];
    if(fence!=NSNull.null){if(((id<MTLCommandBuffer>)fence).status<MTLCommandBufferStatusCompleted)c.arenaWaits++;int e=LRGuestMetalCompleted(c,fence);c.arenaFences[c.arenaIndex]=NSNull.null;if(e)return e;}
    c.vertexArena=arena==NSNull.null?nil:arena;c.vertexOffset=0;
    return 0;
}
// Reuse one bounded staging arena per context instead of allocating a Metal
// buffer for each large guest draw. Offsets remain live until Finish completes.
static int LRMetalBlendFactor(unsigned value,MTLBlendFactor *out){switch(value){
 case 0:*out=MTLBlendFactorZero;break;case 1:*out=MTLBlendFactorOne;break;
 case 0x300:*out=MTLBlendFactorSourceColor;break;case 0x301:*out=MTLBlendFactorOneMinusSourceColor;break;
 case 0x302:*out=MTLBlendFactorSourceAlpha;break;case 0x303:*out=MTLBlendFactorOneMinusSourceAlpha;break;
 case 0x304:*out=MTLBlendFactorDestinationAlpha;break;case 0x305:*out=MTLBlendFactorOneMinusDestinationAlpha;break;
 case 0x306:*out=MTLBlendFactorDestinationColor;break;case 0x307:*out=MTLBlendFactorOneMinusDestinationColor;break;
 case 0x308:*out=MTLBlendFactorSourceAlphaSaturated;break;
 case 0x8001:*out=MTLBlendFactorBlendColor;break;case 0x8002:*out=MTLBlendFactorOneMinusBlendColor;break;
 case 0x8003:*out=MTLBlendFactorBlendAlpha;break;case 0x8004:*out=MTLBlendFactorOneMinusBlendAlpha;break;
 default:return -22;}return 0;}
static int LRMetalBlendOperation(unsigned value,MTLBlendOperation *out){switch(value){case 0x8006:*out=MTLBlendOperationAdd;break;case 0x8007:*out=MTLBlendOperationMin;break;case 0x8008:*out=MTLBlendOperationMax;break;case 0x800a:*out=MTLBlendOperationSubtract;break;case 0x800b:*out=MTLBlendOperationReverseSubtract;break;default:return -22;}return 0;}
static id<MTLTexture> LRGuestColorTarget(LRGuestMetalContext *c,unsigned i){if(!i)return c.target;return !c.boundFramebuffer&&i<c.mrtColors.count?c.mrtColors[i]:nil;}
static MTLColorWriteMask LRGuestColorMask(unsigned m){return ((m&1)?MTLColorWriteMaskRed:0)|((m&2)?MTLColorWriteMaskGreen:0)|((m&4)?MTLColorWriteMaskBlue:0)|((m&8)?MTLColorWriteMaskAlpha:0);}
static int LRGuestMetalBlend(void *context,const struct lr_gpu_blend *r){LRGuestMetalContext *c=(__bridge LRGuestMetalContext *)context;MTLBlendFactor f;MTLBlendOperation o;
 if(r->enabled>1||r->dst_rgb==0x308||r->dst_alpha==0x308||LRMetalBlendFactor(r->src_rgb,&f)||LRMetalBlendFactor(r->dst_rgb,&f)||LRMetalBlendFactor(r->src_alpha,&f)||LRMetalBlendFactor(r->dst_alpha,&f)||LRMetalBlendOperation(r->equation_rgb,&o)||LRMetalBlendOperation(r->equation_alpha,&o))return -22;
 c.blendState=*r;return 0;
}
static id<MTLRenderPipelineState> LRGuestBlendPipeline(LRGuestMetalContext *c,id<MTLRenderPipelineState> base,MTLRenderPipelineDescriptor *descriptor){
 struct lr_gpu_blend r=c.blendState;if(!r.enabled&&c.rasterState.color_mask==15&&!c.mrtColors.count&&descriptor.colorAttachments[0].pixelFormat==c.target.pixelFormat)return base;
 NSString *key=[NSString stringWithFormat:@"%p:%u,%u,%u,%u,%u,%u,%u",(__bridge void *)base,r.enabled,r.src_rgb,r.dst_rgb,r.src_alpha,r.dst_alpha,r.equation_rgb,r.equation_alpha];
 key=[key stringByAppendingFormat:@"-mask%u-format%lu",c.rasterState.color_mask,(unsigned long)c.target.pixelFormat];for(unsigned i=0;i<4;i++)key=[key stringByAppendingFormat:@"-%lu-m%u",(unsigned long)LRGuestColorTarget(c,i).pixelFormat,c.mrtMasks.length>=4*(i+1)?((const uint32_t *)c.mrtMasks.bytes)[i]:15];NSArray *entry=c.blendPipelines[key];if(entry)return entry[1];id<MTLRenderPipelineState> pipeline;
 MTLRenderPipelineDescriptor *d=[descriptor copy];MTLRenderPipelineColorAttachmentDescriptor *a=d.colorAttachments[0];a.pixelFormat=c.target.pixelFormat;a.blendingEnabled=r.enabled;a.writeMask=(MTLColorWriteMask)(((c.rasterState.color_mask&1)?MTLColorWriteMaskRed:0)|((c.rasterState.color_mask&2)?MTLColorWriteMaskGreen:0)|((c.rasterState.color_mask&4)?MTLColorWriteMaskBlue:0)|((c.rasterState.color_mask&8)?MTLColorWriteMaskAlpha:0));
 MTLBlendFactor f;LRMetalBlendFactor(r.src_rgb,&f);a.sourceRGBBlendFactor=f;LRMetalBlendFactor(r.dst_rgb,&f);a.destinationRGBBlendFactor=f;LRMetalBlendFactor(r.src_alpha,&f);a.sourceAlphaBlendFactor=f;LRMetalBlendFactor(r.dst_alpha,&f);a.destinationAlphaBlendFactor=f;
 MTLBlendOperation o;LRMetalBlendOperation(r.equation_rgb,&o);a.rgbBlendOperation=o;LRMetalBlendOperation(r.equation_alpha,&o);a.alphaBlendOperation=o;
 MTLRenderPipelineColorAttachmentDescriptor *prototype=[a copy];for(unsigned i=0;i<4;i++){id<MTLTexture> target=LRGuestColorTarget(c,i);d.colorAttachments[i].pixelFormat=target?target.pixelFormat:MTLPixelFormatInvalid;if(!target)continue;if(i)d.colorAttachments[i]=[prototype copy];d.colorAttachments[i].pixelFormat=target.pixelFormat;unsigned mask=c.mrtMasks.length>=4*(i+1)?((const uint32_t *)c.mrtMasks.bytes)[i]:15;d.colorAttachments[i].writeMask=LRGuestColorMask(mask&(i?15:c.rasterState.color_mask));if(target.pixelFormat==MTLPixelFormatRGBA32Uint||target.pixelFormat==MTLPixelFormatRGBA32Sint||target.pixelFormat==MTLPixelFormatRGBA16Uint||target.pixelFormat==MTLPixelFormatRGBA16Sint)d.colorAttachments[i].blendingEnabled=NO;}
 NSError *error=nil;pipeline=[c.device newRenderPipelineStateWithDescriptor:d error:&error];if(!pipeline){fprintf(stderr,"GUEST_METAL_BLEND_PIPELINE_ERROR %s\n",error.localizedDescription.UTF8String);return nil;}
 if(c.blendPipelines.count>=64)[c.blendPipelines removeObjectForKey:c.blendPipelines.allKeys.firstObject];c.blendPipelines[key]=@[base,pipeline];return pipeline;
}
static void LRGuestSetBlendColor(LRGuestMetalContext *c,id<MTLRenderCommandEncoder> e){struct lr_gpu_blend r=c.blendState;[e setBlendColorRed:r.color[0] green:r.color[1] blue:r.color[2] alpha:r.color[3]];}
static int LRGuestMetalVertices(LRGuestMetalContext *c,id<MTLRenderCommandEncoder> encoder,const void *vertices,size_t bytes) {
    if(bytes<=4096){[encoder setVertexBytes:vertices length:bytes atIndex:0];c.vertexUploadBytes+=bytes;return 0;}
    const NSUInteger capacity=1048576;
    NSUInteger offset=(c.vertexOffset+255)&~(NSUInteger)255;
    if(bytes>capacity||offset>capacity-bytes)return -12;
    if(!c.vertexArena){c.vertexArena=[c.device newBufferWithLength:capacity options:MTLResourceStorageModeShared];if(!c.vertexArena)return -12;c.vertexAllocations++;}
    memcpy((uint8_t*)c.vertexArena.contents+offset,vertices,bytes);
    [encoder setVertexBuffer:c.vertexArena offset:offset atIndex:0];
    c.vertexOffset=offset+bytes;c.vertexUploadBytes+=bytes;return 0;
}
static int LRGuestMetalViewport(void *context,const struct lr_gpu_rectangle *area){
 LRGuestMetalContext *c=(__bridge LRGuestMetalContext*)context;c.viewport=*area;return 0;
}
static int LRGuestMetalSetViewport(LRGuestMetalContext *c,id<MTLRenderCommandEncoder> encoder){
 struct lr_gpu_rectangle v=c.viewport;if(!v.width||!v.height||v.x>=(int64_t)c.target.width||v.y>=(int64_t)c.target.height||(int64_t)v.x+v.width<=0||(int64_t)v.y+v.height<=0)return 0;
 struct lr_gpu_raster_state r=c.rasterState;double znear=(r.extensions&LR_GPU_RASTER_DEPTH_RANGE)?r.depth_near:0,zfar=(r.extensions&LR_GPU_RASTER_DEPTH_RANGE)?r.depth_far:1;   // glDepthRange
 [encoder setViewport:(MTLViewport){v.x,(double)c.target.height-v.y-v.height,v.width,v.height,znear,zfar}];return 1;
}
static int LRGuestMetalReadRegion(void *context,void *pixels,const struct lr_gpu_rectangle *area){
 @autoreleasepool {LRGuestMetalContext *c=(__bridge LRGuestMetalContext*)context;int error=LRGuestMetalFinish(c);if(error)return error;
  if(!c.painted||area->x<0||area->y<0||area->width>c.target.width||area->height>c.target.height||(NSUInteger)area->x>c.target.width-area->width||(NSUInteger)area->y>c.target.height-area->height)return -22;
  [c.target getBytes:pixels bytesPerRow:area->width*4 fromRegion:MTLRegionMake2D(area->x,c.target.height-area->y-area->height,area->width,area->height) mipmapLevel:0];
  uint8_t row[LR_GPU_MAX_DIMENSION*4];size_t stride=area->width*4;
  for(unsigned y=0;y<area->height/2;y++){uint8_t *a=(uint8_t*)pixels+y*stride,*b=(uint8_t*)pixels+(area->height-1-y)*stride;memcpy(row,a,stride);memcpy(a,b,stride);memcpy(b,row,stride);}
  c.readbacks++;return 0;
 }
}
static MTLCompareFunction LRStencilCompare(unsigned value){static const MTLCompareFunction f[]={MTLCompareFunctionNever,MTLCompareFunctionLess,MTLCompareFunctionEqual,MTLCompareFunctionLessEqual,MTLCompareFunctionGreater,MTLCompareFunctionNotEqual,MTLCompareFunctionGreaterEqual,MTLCompareFunctionAlways};return value>=0x0200&&value<=0x0207?f[value-0x0200]:MTLCompareFunctionAlways;}
static MTLStencilOperation LRStencilOperation(unsigned value){switch(value){case 0x1e00:return MTLStencilOperationKeep;case 0:return MTLStencilOperationZero;case 0x1e01:return MTLStencilOperationReplace;case 0x1e02:return MTLStencilOperationIncrementClamp;case 0x1e03:return MTLStencilOperationDecrementClamp;case 0x150a:return MTLStencilOperationInvert;case 0x8507:return MTLStencilOperationIncrementWrap;case 0x8508:return MTLStencilOperationDecrementWrap;default:return MTLStencilOperationKeep;}}
// Include every descriptor field; references are encoder state, not pipeline state.
// Bounded eviction avoids unbounded guest-driven state cache growth.
static id<MTLDepthStencilState> LRGuestDepthPipeline(LRGuestMetalContext *c,unsigned flags){
 struct lr_gpu_stencil_state stencil=c.stencilState;stencil.clear=stencil.clear_value=0;
 stencil.front.reference=stencil.back.reference=0;
 uint32_t keyData[2+sizeof stencil/4]={flags&LR_GPU_DEPTH_TEST_BIT?c.depthState.function:0x0207,flags&LR_GPU_DEPTH_TEST_BIT?c.depthState.write_mask:0};
 memcpy(keyData+2,&stencil,sizeof stencil);NSData *key=[NSData dataWithBytes:keyData length:sizeof keyData];
 id<MTLDepthStencilState> state=c.depthStates[key];if(state)return state;
 MTLDepthStencilDescriptor *d=[MTLDepthStencilDescriptor new];d.depthCompareFunction=LRStencilCompare(keyData[0]);d.depthWriteEnabled=keyData[1];
 if(stencil.enabled){MTLStencilDescriptor *faces[2]={[MTLStencilDescriptor new],[MTLStencilDescriptor new]};
  const struct lr_gpu_stencil_face *values[2]={&stencil.front,&stencil.back};
  for(unsigned i=0;i<2;i++){const struct lr_gpu_stencil_face *v=values[i];MTLStencilDescriptor *f=faces[i];f.stencilCompareFunction=LRStencilCompare(v->function);f.stencilFailureOperation=LRStencilOperation(v->fail);f.depthFailureOperation=LRStencilOperation(v->depth_fail);f.depthStencilPassOperation=LRStencilOperation(v->depth_pass);f.readMask=v->read_mask;f.writeMask=v->write_mask;}d.frontFaceStencil=faces[0];d.backFaceStencil=faces[1];}
 state=[c.device newDepthStencilStateWithDescriptor:d];if(state){if(c.depthStates.count>=128)[c.depthStates removeAllObjects];c.depthStates[key]=state;}return state;
}
// Transfers use tightly packed bottom-up guest rows and aligned top-down Metal rows.
// Only the companion default target is addressable; guest FBO attachments remain separate.
static int LRGuestMetalStencilTransfer(void *context,void *pixels,unsigned w,unsigned h,unsigned read){
 LRGuestMetalContext *c=(__bridge LRGuestMetalContext *)context;{int released=LRGuestResidentRelease(c);if(released)return released;}if(c.boundFramebuffer||!c.defaultStencil||w!=c.defaultStencil.width||h!=c.defaultStencil.height)return -22;
 int error=LRGuestMetalFinish(c);if(error)return error;NSUInteger pitch=((NSUInteger)w+255)&~(NSUInteger)255,size=pitch*h;
 if(c.depthTransfer.length<size){id<MTLBuffer> b=[c.device newBufferWithLength:size options:MTLResourceStorageModeShared];if(!b)return -12;c.depthTransfer=b;}
 if(!read)for(unsigned y=0;y<h;y++)memcpy((char *)c.depthTransfer.contents+(h-y-1)*pitch,(char *)pixels+(size_t)y*w,w);
 id<MTLCommandBuffer> command=[c.queue commandBuffer];id<MTLBlitCommandEncoder> blit=[command blitCommandEncoder];if(!blit)return -5;
 if(read)[blit copyFromTexture:c.defaultStencil sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0,0,0) sourceSize:MTLSizeMake(w,h,1) toBuffer:c.depthTransfer destinationOffset:0 destinationBytesPerRow:pitch destinationBytesPerImage:size];
 else [blit copyFromBuffer:c.depthTransfer sourceOffset:0 sourceBytesPerRow:pitch sourceBytesPerImage:size sourceSize:MTLSizeMake(w,h,1) toTexture:c.defaultStencil destinationSlice:0 destinationLevel:0 destinationOrigin:MTLOriginMake(0,0,0)];
 [blit endEncoding];[command commit];[command waitUntilCompleted];if(command.status!=MTLCommandBufferStatusCompleted){fprintf(stderr,"GUEST_METAL_STENCIL_TRANSFER_ERROR %s\n",command.error.localizedDescription.UTF8String);return -5;}
 if(read){for(unsigned y=0;y<h;y++)memcpy((char *)pixels+(size_t)y*w,(char *)c.depthTransfer.contents+(h-y-1)*pitch,w);c.readbacks++;}else c.defaultStencilPainted=YES;return 0;
}
static int LRGuestMetalStencilState(void *context,const struct lr_gpu_stencil_state *state){
 LRGuestMetalContext *c=(__bridge LRGuestMetalContext *)context;if(!lr_gpu_stencil_state_valid(state)||c.boundFramebuffer)return -22;
 if(state->clear){size_t bytes=c.defaultStencil.width*c.defaultStencil.height;NSMutableData *data=[NSMutableData dataWithLength:bytes];if(!data)return -12;memset(data.mutableBytes,state->clear_value,bytes);int error=LRGuestMetalStencilTransfer(context,data.mutableBytes,(unsigned)c.defaultStencil.width,(unsigned)c.defaultStencil.height,0);if(error)return error;}
 struct lr_gpu_stencil_state copy=*state;copy.clear=0;c.stencilState=copy;return 0;
}
static int LRGuestMetalDepthState(void *context,const struct lr_gpu_depth_state *state){
 if(state->function<0x0200||state->function>0x0207||state->write_mask>1||!isfinite(state->clear_depth)||state->clear_depth<0||state->clear_depth>1)return -22;
 LRGuestMetalContext *c=(__bridge LRGuestMetalContext *)context;c.depthState=*state;return 0;
}
static int LRGuestMetalRasterState(void *context,const struct lr_gpu_raster_state *r){
 if((r->depth_bits&&r->depth_bits!=16&&r->depth_bits!=24&&r->depth_bits!=32)||r->framebuffer_srgb>1||r->scissor>1||r->color_mask>15||r->width>INT32_MAX||r->height>INT32_MAX||(r->cull_face!=0x0404&&r->cull_face!=0x0405&&r->cull_face!=0x0408)||(r->front_face!=0x0900&&r->front_face!=0x0901))return -22;
 LRGuestMetalContext *c=(__bridge LRGuestMetalContext *)context;if(r->framebuffer_srgb&&(c.boundFramebuffer||(c.residentBound&&!c.residentSrgb)))return -22;if(c.rasterState.framebuffer_srgb!=r->framebuffer_srgb){int error=LRGuestMetalFinish(c);if(error)return error;}c.rasterState=*r;return LRGuestSelectTarget(c);
}
static BOOL LRGuestRaster(LRGuestMetalContext *c,id<MTLRenderCommandEncoder> e,unsigned flags){
 struct lr_gpu_raster_state r=c.rasterState;
 if((flags&LR_GPU_CULL_BIT)&&r.cull_face==0x0408)return NO;
 [e setCullMode:(flags&LR_GPU_CULL_BIT)?(r.cull_face==0x0404?MTLCullModeFront:MTLCullModeBack):MTLCullModeNone];
 [e setFrontFacingWinding:r.front_face==0x0900?MTLWindingClockwise:MTLWindingCounterClockwise];
 // glPolygonOffset: GL offset = factor * max depth slope + units * r -> Metal slope scale and constant bias.
 if(r.extensions&LR_GPU_RASTER_POLYGON_OFFSET)[e setDepthBias:r.offset_units slopeScale:r.offset_factor clamp:0];else [e setDepthBias:0 slopeScale:0 clamp:0];
 int64_t x=0,y=0,right=c.target.width,top=c.target.height;
 if(r.scissor){x=MAX((int64_t)0,r.x);y=MAX((int64_t)0,r.y);right=MIN((int64_t)c.target.width,(int64_t)r.x+r.width);top=MIN((int64_t)c.target.height,(int64_t)r.y+r.height);}
 if(right<=x||top<=y)return NO;
 [e setScissorRect:(MTLScissorRect){x,c.target.height-top,right-x,top-y}];return YES;
}
static int LRGuestMetalImportColor(void *context,const void *data,unsigned w,unsigned h){
 LRGuestMetalContext *c=(__bridge LRGuestMetalContext *)context;{int released=LRGuestResidentRelease(c);if(released)return released;}
 if(c.boundFramebuffer||w!=c.defaultTarget.width||h!=c.defaultTarget.height)return -22;
 int error=LRGuestMetalFinish(c);if(error)return error;
 for(unsigned row=0;row<h;row++)[c.defaultTarget replaceRegion:MTLRegionMake2D(0,h-row-1,w,1) mipmapLevel:0 withBytes:(const char *)data+(size_t)row*w*4 bytesPerRow:w*4];
 c.painted=c.defaultPainted=YES;return LRGuestSelectTarget(c);
}
static int LRGuestMetalImportFrame(void *context,const void *data,unsigned w,unsigned h,unsigned format){
 LRGuestMetalContext *c=(__bridge LRGuestMetalContext *)context;{int released=LRGuestResidentRelease(c);if(released)return released;}MTLPixelFormat pixel=format==0x8058?MTLPixelFormatRGBA8Unorm:format==0x881a?MTLPixelFormatRGBA16Float:MTLPixelFormatInvalid;
 if(pixel==MTLPixelFormatInvalid||c.boundFramebuffer||w!=c.defaultTarget.width||h!=c.defaultTarget.height)return -22;
 int error=LRGuestMetalFinish(c);if(error)return error;
 if(c.defaultTarget.pixelFormat!=pixel){MTLTextureDescriptor *d=[MTLTextureDescriptor texture2DDescriptorWithPixelFormat:pixel width:w height:h mipmapped:NO];d.storageMode=MTLStorageModeShared;d.usage=MTLTextureUsageRenderTarget|MTLTextureUsagePixelFormatView;id<MTLTexture> texture=[c.device newTextureWithDescriptor:d];if(!texture)return -12;c.defaultTarget=texture;c.mrtColors=nil;c.mrtMasks=nil;c.defaultSrgbView=nil;c.target=texture;}
 c.mrtColors=nil;c.mrtMasks=nil;unsigned bpp=format==0x881a?8:4;for(unsigned row=0;row<h;row++)[c.defaultTarget replaceRegion:MTLRegionMake2D(0,h-row-1,w,1) mipmapLevel:0 withBytes:(const char *)data+(size_t)row*w*bpp bytesPerRow:w*bpp];c.painted=c.defaultPainted=YES;return LRGuestSelectTarget(c);
}
static int LRGuestMetalReadFrame(void *context,void *data,unsigned w,unsigned h,unsigned format){
 LRGuestMetalContext *c=(__bridge LRGuestMetalContext *)context;{int released=LRGuestResidentRelease(c);if(released)return released;}MTLPixelFormat pixel=format==0x8058?MTLPixelFormatRGBA8Unorm:format==0x881a?MTLPixelFormatRGBA16Float:MTLPixelFormatInvalid;
 if(pixel==MTLPixelFormatInvalid||c.boundFramebuffer||pixel!=c.defaultTarget.pixelFormat||w!=c.defaultTarget.width||h!=c.defaultTarget.height)return -22;
 int error=LRGuestMetalFinish(c);if(error)return error;unsigned bpp=format==0x881a?8:4;for(unsigned row=0;row<h;row++)[c.defaultTarget getBytes:(char *)data+(size_t)row*w*bpp bytesPerRow:w*bpp fromRegion:MTLRegionMake2D(0,h-row-1,w,1) mipmapLevel:0];c.readbacks++;return 0;
}
static MTLPixelFormat LRGuestInteropFormat(unsigned format){switch(format){case 0x8058:return MTLPixelFormatRGBA8Unorm;case 0x881a:return MTLPixelFormatRGBA16Float;case 0x8d76:return MTLPixelFormatRGBA16Uint;case 0x8d88:return MTLPixelFormatRGBA16Sint;case 0x8d70:return MTLPixelFormatRGBA32Uint;case 0x8d82:return MTLPixelFormatRGBA32Sint;default:return MTLPixelFormatInvalid;}}
static int LRGuestMetalImportMrt(void *context,const struct lr_gpu_mrt *r,size_t bytes,unsigned w,unsigned h){
 LRGuestMetalContext *c=(__bridge LRGuestMetalContext *)context;{int released=LRGuestResidentRelease(c);if(released)return released;}if(c.boundFramebuffer||!r->count||r->count>4||w!=c.defaultTarget.width||h!=c.defaultTarget.height)return -22;
 size_t at=sizeof *r;for(unsigned i=0;i<r->count;i++){MTLPixelFormat format=LRGuestInteropFormat(r->color[i].format);unsigned pixel=r->color[i].format==0x8058?4:(r->color[i].format==0x881a||r->color[i].format==0x8d76||r->color[i].format==0x8d88)?8:16;if(format==MTLPixelFormatInvalid||r->color[i].mask>15||r->color[i].offset!=at||r->color[i].bytes!=(size_t)w*h*pixel||r->color[i].bytes>bytes-at)return -22;at+=r->color[i].bytes;}if(at!=bytes)return -22;
 int error=LRGuestMetalFinish(c);if(error)return error;NSMutableArray<id<MTLTexture>> *targets=[NSMutableArray new];uint32_t masks[4]={0};
 // Allocate every replacement before mutating the existing layout.
 for(unsigned i=0;i<r->count;i++){MTLPixelFormat format=LRGuestInteropFormat(r->color[i].format);id<MTLTexture> old=i<c.mrtColors.count?c.mrtColors[i]:!i?c.defaultTarget:nil;id<MTLTexture> target=old;
 if(!old||old.pixelFormat!=format||old.width!=w||old.height!=h){MTLTextureDescriptor *d=[MTLTextureDescriptor texture2DDescriptorWithPixelFormat:format width:w height:h mipmapped:NO];d.storageMode=MTLStorageModeShared;d.usage=MTLTextureUsageRenderTarget|MTLTextureUsagePixelFormatView;target=[c.device newTextureWithDescriptor:d];if(!target)return -12;}[targets addObject:target];masks[i]=r->color[i].mask;}
 for(unsigned i=0;i<r->count;i++){unsigned pixel=r->color[i].format==0x8058?4:(r->color[i].format==0x881a||r->color[i].format==0x8d76||r->color[i].format==0x8d88)?8:16;const char *data=(const char *)r+r->color[i].offset;for(unsigned y=0;y<h;y++)[targets[i] replaceRegion:MTLRegionMake2D(0,h-y-1,w,1) mipmapLevel:0 withBytes:data+(size_t)y*w*pixel bytesPerRow:w*pixel];}
 c.mrtColors=targets;c.mrtMasks=[NSData dataWithBytes:masks length:sizeof masks];c.defaultTarget=targets[0];c.defaultSrgbView=nil;c.painted=c.defaultPainted=YES;return LRGuestSelectTarget(c);
}
static int LRGuestMetalReadMrt(void *context,void *pixels,unsigned index,unsigned w,unsigned h,unsigned format){LRGuestMetalContext *c=(__bridge LRGuestMetalContext *)context;{int released=LRGuestResidentRelease(c);if(released)return released;}if(c.boundFramebuffer||index>=c.mrtColors.count)return -22;id<MTLTexture> texture=c.mrtColors[index];if(texture.pixelFormat!=LRGuestInteropFormat(format)||w!=texture.width||h!=texture.height)return -22;int error=LRGuestMetalFinish(c);if(error)return error;unsigned pixel=format==0x8058?4:(format==0x881a||format==0x8d76||format==0x8d88)?8:16;for(unsigned y=0;y<h;y++)[texture getBytes:(char *)pixels+(size_t)y*w*pixel bytesPerRow:w*pixel fromRegion:MTLRegionMake2D(0,h-y-1,w,1) mipmapLevel:0];c.readbacks++;return 0;}
static int LRGuestMetalDepthTransfer(void *context,void *pixels,unsigned w,unsigned h,unsigned read){
 LRGuestMetalContext *c=(__bridge LRGuestMetalContext *)context;{int released=LRGuestResidentRelease(c);if(released)return released;}if(c.boundFramebuffer||w!=c.defaultDepth.width||h!=c.defaultDepth.height)return -22;
 int error=LRGuestMetalFinish(c);if(error)return error;NSUInteger pitch=((NSUInteger)w*4+255)&~(NSUInteger)255,size=pitch*h;
 if(c.depthTransfer.length<size){id<MTLBuffer> buffer=[c.device newBufferWithLength:size options:MTLResourceStorageModeShared];if(!buffer)return -12;c.depthTransfer=buffer;}
 if(!read)for(unsigned y=0;y<h;y++)memcpy((char *)c.depthTransfer.contents+(h-y-1)*pitch,(char *)pixels+(size_t)y*w*4,w*4);
 id<MTLCommandBuffer> command=[c.queue commandBuffer];id<MTLBlitCommandEncoder> blit=[command blitCommandEncoder];if(!blit)return -5;
 if(read)[blit copyFromTexture:c.defaultDepth sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0,0,0) sourceSize:MTLSizeMake(w,h,1) toBuffer:c.depthTransfer destinationOffset:0 destinationBytesPerRow:pitch destinationBytesPerImage:size];
 else [blit copyFromBuffer:c.depthTransfer sourceOffset:0 sourceBytesPerRow:pitch sourceBytesPerImage:size sourceSize:MTLSizeMake(w,h,1) toTexture:c.defaultDepth destinationSlice:0 destinationLevel:0 destinationOrigin:MTLOriginMake(0,0,0)];
 [blit endEncoding];[command commit];[command waitUntilCompleted];if(command.status!=MTLCommandBufferStatusCompleted){fprintf(stderr,"GUEST_METAL_DEPTH_TRANSFER_ERROR %s\n",command.error.localizedDescription.UTF8String);return -5;}
 if(read){for(unsigned y=0;y<h;y++)memcpy((char *)pixels+(size_t)y*w*4,(char *)c.depthTransfer.contents+(h-y-1)*pitch,w*4);c.readbacks++;}return 0;
}
static int LRGuestMetalDraw(void *context,const struct lr_gpu_vertex *vertices,unsigned count,unsigned flags,const float clear[4]) {
    @autoreleasepool {
#if TARGET_OS_IPHONE
        if(NSThread.isMainThread)return -11;
#endif
        LRGuestMetalContext *c=(__bridge LRGuestMetalContext *)context;
        if(c.mrtColors.count>1)return -95;
        if(!c.target||(c.boundFramebuffer&&LRGuestFramebufferStatus(c.framebuffers[@(c.boundFramebuffer)])!=0x8cd5))return -22;
        if(c.boundFramebuffer&&!c.framebuffers[@(c.boundFramebuffer)].depth)flags&=~LR_GPU_DEPTH_TEST_BIT;
        if(c.pendingBytes+(size_t)count*sizeof *vertices>1048576||((c.vertexOffset+255)&~(NSUInteger)255)+(size_t)count*sizeof *vertices>1048576){int e=LRGuestMetalSubmit(c);if(e)return e;}
        if(!c.pending){c.pending=[c.queue commandBuffer];if(!c.pending)return -5;}
        if(c.encoder&&(flags&(LR_GPU_CLEAR_BIT|LR_GPU_DEPTH_CLEAR_BIT))){[c.encoder endEncoding];c.encoder=nil;}
        if(!c.encoder){
            MTLRenderPassDescriptor *pass=[MTLRenderPassDescriptor renderPassDescriptor];
            pass.depthAttachment.texture=c.depth;
            pass.depthAttachment.loadAction=(!c.painted||(flags&LR_GPU_DEPTH_CLEAR_BIT))?MTLLoadActionClear:MTLLoadActionLoad;
            pass.depthAttachment.storeAction=MTLStoreActionStore;pass.depthAttachment.clearDepth=c.depthState.clear_depth;
            pass.stencilAttachment.texture=c.stencil;pass.stencilAttachment.loadAction=(c.boundFramebuffer?c.offscreenStencilPainted:c.defaultStencilPainted)?MTLLoadActionLoad:MTLLoadActionClear;pass.stencilAttachment.storeAction=MTLStoreActionStore;pass.stencilAttachment.clearStencil=0;
            pass.colorAttachments[0].texture=c.target;
            pass.colorAttachments[0].loadAction=(!c.painted||(flags&LR_GPU_CLEAR_BIT))?MTLLoadActionClear:MTLLoadActionLoad;
            pass.colorAttachments[0].storeAction=MTLStoreActionStore;
            pass.colorAttachments[0].clearColor=MTLClearColorMake(clear[0],clear[1],clear[2],clear[3]);
            c.encoder=[c.pending renderCommandEncoderWithDescriptor:pass];if(!c.encoder)return -5;
        }
        id<MTLRenderCommandEncoder> encoder=c.encoder;
        id<MTLDepthStencilState> depth=LRGuestDepthPipeline(c,flags);if(!depth)return -12;[encoder setDepthStencilState:depth];[encoder setStencilFrontReferenceValue:c.stencilState.front.reference backReferenceValue:c.stencilState.back.reference];if(c.boundFramebuffer)c.offscreenStencilPainted=YES;else c.defaultStencilPainted=YES;
        BOOL raster=LRGuestRaster(c,encoder,flags);id<MTLRenderPipelineState> pipeline=LRGuestBlendPipeline(c,c.pipeline,c.descriptor);if(!pipeline)return -5;[encoder setRenderPipelineState:pipeline];LRGuestSetBlendColor(c,encoder);
        if(flags&LR_GPU_TRIANGLES_BIT){
            size_t bytes=count*sizeof *vertices;
            int error=LRGuestMetalVertices(c,encoder,vertices,bytes);if(error)return error;
            if(raster&&LRGuestMetalSetViewport(c,encoder))[encoder drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:count];
        }
        c.pendingBytes+=(size_t)count*sizeof *vertices;c.pendingDraws++;c.painted=YES;
        return c.pendingDraws>=64?LRGuestMetalSubmit(c):0;
    }
}
static int LRGuestMetalRead(void *context,void *pixels,unsigned width,unsigned height,unsigned x11BGRA) {
    @autoreleasepool {
        LRGuestMetalContext *c=(__bridge LRGuestMetalContext *)context;
        int result=LRGuestMetalFinish(c);if(result)return result;
        id<MTLTexture> target=x11BGRA?c.defaultTarget:c.target;BOOL painted=x11BGRA?c.defaultPainted:c.painted;
        if(!painted || width!=target.width || height!=target.height)return -22;
        // GLES glReadPixels starts at the lower-left; Metal texture row 0 is top.
        [target getBytes:pixels bytesPerRow:width*4 fromRegion:MTLRegionMake2D(0,0,width,height) mipmapLevel:0];
        if(x11BGRA){
            // X11 TrueColor top-left BGRA: native channel shuffle replaces the
            // interpreted per-pixel conversion and a separate guest RGBA buffer.
            uint8_t *p=pixels;size_t n=(size_t)width*height,i=0;
#if defined(__aarch64__)
            for(;i+16<=n;i+=16){uint8x16x4_t v=vld4q_u8(p+i*4);uint8x16_t r=v.val[0];v.val[0]=v.val[2];v.val[2]=r;v.val[3]=vdupq_n_u8(255);vst4q_u8(p+i*4,v);}
#endif
            for(;i<n;i++){uint8_t r=p[i*4];p[i*4]=p[i*4+2];p[i*4+2]=r;p[i*4+3]=255;}
            c.readbacks++;return 0;
        }
        uint8_t row[LR_GPU_MAX_DIMENSION*4];
        for(unsigned y=0;y<height/2;y++) {
            uint8_t *top=(uint8_t *)pixels+(size_t)y*width*4,*bottom=(uint8_t *)pixels+(size_t)(height-1-y)*width*4;
            memcpy(row,top,width*4);memcpy(top,bottom,width*4);memcpy(bottom,row,width*4);
        }
        c.readbacks++;return 0;
    }
}
static int LRGuestMetalSync(void *context){return LRGuestMetalFinish((__bridge LRGuestMetalContext *)context);}
static int LRGuestMetalInfo(void *context,uint64_t *submissions,uint64_t *readbacks,uint64_t *ns) {
    LRGuestMetalContext *c=(__bridge LRGuestMetalContext *)context;
    int result=LRGuestMetalFinish(c);if(result)return result;
    *submissions=c.submissions;*readbacks=c.readbacks;*ns=c.gpuNanoseconds;return 0;
}
static void LRGuestMetalDestroy(void *context) { @autoreleasepool { LRGuestMetalFinish((__bridge LRGuestMetalContext *)context);id released=CFBridgingRelease(context);(void)released; } }
static int LRGuestMetalCompile(void *context,struct lr_gl_compile *r) {
 @autoreleasepool {
    LRGuestMetalContext *c=(__bridge LRGuestMetalContext *)context;
    NSValue *old=c.shaders[@(r->name)];if(old){lr_glsl_shader_destroy(old.pointerValue);[c.shaders removeObjectForKey:@(r->name)];}
    r->status=0;memset(r->log,0,sizeof r->log);
    void *shader=lr_glsl_compile(r->stage,r->source,r->log);
    if(shader){c.shaders[@(r->name)]=[NSValue valueWithPointer:shader];r->status=1;}
    return 0; // A compiler rejection is GL compile status, not ioctl failure.
 }
}
static int LRGuestMetalLink(void *context,struct lr_gl_link *r) {
 @autoreleasepool {
    LRGuestMetalContext *c=(__bridge LRGuestMetalContext *)context;
    int error=LRGuestMetalFinish(c);if(error)return error;
    r->status=0;
    void *compiled=lr_glsl_link(c.shaders[@(r->vertex)].pointerValue,c.shaders[@(r->fragment)].pointerValue,r);
    if(!compiled)return 0;
    NSString *vertex=[NSString stringWithUTF8String:lr_glsl_msl(compiled,0)],*fragment=[NSString stringWithUTF8String:lr_glsl_msl(compiled,1)];
    if(!vertex||!fragment||vertex.length>262144||fragment.length>262144){lr_glsl_program_destroy(compiled);r->status=0;snprintf(r->log,sizeof r->log,"Translated shader exceeds limit");return 0;}
    NSString *key=[NSString stringWithFormat:@"%@\n%@",vertex,fragment];
    LRGuestMetalProgram *program=c.pipelineCache[key];BOOL hit=program!=nil;
    if(!program){
      NSError *e=nil;id<MTLLibrary> vl=[c.device newLibraryWithSource:vertex options:nil error:&e];
      id<MTLLibrary> fl=vl?[c.device newLibraryWithSource:fragment options:nil error:&e]:nil;
      MTLRenderPipelineDescriptor *d=[MTLRenderPipelineDescriptor new];d.vertexFunction=[vl newFunctionWithName:@"main0"];d.fragmentFunction=[fl newFunctionWithName:@"main0"];
      for(unsigned i=0;i<4;i++){id<MTLTexture> target=LRGuestColorTarget(c,i);d.colorAttachments[i].pixelFormat=target?target.pixelFormat:MTLPixelFormatInvalid;}d.depthAttachmentPixelFormat=MTLPixelFormatDepth32Float;d.stencilAttachmentPixelFormat=MTLPixelFormatStencil8;
      MTLVertexDescriptor *vd=[MTLVertexDescriptor vertexDescriptor];vd.layouts[0].stride=sizeof(struct lr_gl_vertex);
      for(unsigned i=0;i<r->attributes;i++){unsigned type=r->attribute[i].type,columns=type==0x8b5c?4:1;for(unsigned column=0;column<columns;column++){unsigned slot=r->attribute[i].location+column;
       unsigned n=columns==4?4:lr_gl_type_components(type),kind=lr_gl_type_kind(type);
       vd.attributes[slot].format=kind==1?(n==1?MTLVertexFormatInt:n==2?MTLVertexFormatInt2:n==3?MTLVertexFormatInt3:MTLVertexFormatInt4):kind==2?(n==1?MTLVertexFormatUInt:n==2?MTLVertexFormatUInt2:n==3?MTLVertexFormatUInt3:MTLVertexFormatUInt4):(n==1?MTLVertexFormatFloat:n==2?MTLVertexFormatFloat2:n==3?MTLVertexFormatFloat3:MTLVertexFormatFloat4);
       vd.attributes[slot].offset=slot*16;vd.attributes[slot].bufferIndex=0;
      }}d.vertexDescriptor=vd;
      id<MTLRenderPipelineState> pipeline=(d.vertexFunction&&d.fragmentFunction)?[c.device newRenderPipelineStateWithDescriptor:d error:&e]:nil;
      if(!pipeline){r->status=0;snprintf(r->log,sizeof r->log,"Metal pipeline: %s",e.localizedDescription.UTF8String?:"missing entry point");lr_glsl_program_destroy(compiled);return 0;}
      program=[LRGuestMetalProgram new];program.pipeline=pipeline;program.descriptor=d;program.reflection=*r;
      if(c.pipelineCache.count>=LR_GL_MAX_PROGRAMS)[c.pipelineCache removeObjectForKey:c.pipelineCache.allKeys.firstObject];c.pipelineCache[key]=program;
    }
    // Pipeline cache shares immutable GPU state only. Reflection and mutable
    // block storage belong to this program, including on cache hits/relinks.
    LRGuestMetalProgram *instance=[LRGuestMetalProgram new];instance.pipeline=program.pipeline;instance.descriptor=program.descriptor;instance.reflection=*r;
    NSMutableData *blocks=[NSMutableData dataWithLength:sizeof(struct lr_gl_blocks)];struct lr_gl_blocks *blockReflection=blocks.mutableBytes;lr_glsl_blocks(compiled,blockReflection);blockReflection->program=r->name;instance.blocksData=blocks;
    struct lr_gl_uniform_extents extents;lr_glsl_uniform_extents(compiled,&extents);instance.extents=extents;instance.arrayData=[NSMutableDictionary new];
    for(unsigned i=0;i<extents.count;i++)if(extents.uniform[i].elements>1)instance.arrayData[@(i)]=[NSMutableData dataWithLength:extents.uniform[i].elements*extents.uniform[i].stride];
    struct lr_gl_storages storages;lr_glsl_storages(compiled,&storages);instance.storages=storages;instance.storageBuffers=[NSMutableDictionary new];
    c.programs[@(r->name)]=instance;[c.uniformBlockData removeObjectForKey:@(r->name)];lr_glsl_program_destroy(compiled);
    r->status=1;static atomic_uint reports;if(atomic_fetch_add(&reports,1)<32)fprintf(stderr,"GUEST_METAL_PROGRAM_LINK attributes=%u uniforms=%u pipeline_cache_hit=%d\n",r->attributes,r->uniforms,hit);
    return 0;
 }
}
static MTLSamplerAddressMode LRTextureAddress(unsigned value){return value==0x2901?MTLSamplerAddressModeRepeat:value==0x8370?MTLSamplerAddressModeMirrorRepeat:MTLSamplerAddressModeClampToEdge;}
static id<MTLSamplerState> LRTextureSampler(id<MTLDevice> device,const struct lr_gpu_texture *p){
 MTLSamplerDescriptor *d=[MTLSamplerDescriptor new];d.sAddressMode=LRTextureAddress(p->wrap_s);d.tAddressMode=LRTextureAddress(p->wrap_t);
 d.magFilter=p->mag_filter==0x2601?MTLSamplerMinMagFilterLinear:MTLSamplerMinMagFilterNearest;
 d.minFilter=(p->min_filter==0x2601||p->min_filter==0x2701||p->min_filter==0x2703)?MTLSamplerMinMagFilterLinear:MTLSamplerMinMagFilterNearest;
 d.mipFilter=p->min_filter>=0x2702?MTLSamplerMipFilterLinear:p->min_filter>=0x2700?MTLSamplerMipFilterNearest:MTLSamplerMipFilterNotMipmapped;
 return [device newSamplerStateWithDescriptor:d];
}
static int LRGuestMetalTextureUpdate(void *context,const struct lr_gpu_texture *p,unsigned flags){
 @autoreleasepool {LRGuestMetalContext *c=(__bridge LRGuestMetalContext*)context;
  unsigned format=flags>>8;flags&=255;if(!format)format=0x1908;
  LRGuestMetalTexture *old=c.textures[@(p->name)];
  if(flags==LR_GPU_TEXTURE_STORAGE_CLEAR){int error=LRGuestMetalFinish(c);if(error)return error;old.texture=nil;old.renderTexture=nil;old.bytes=0;old.painted=NO;c.textureBytes=LRGuestTextureStorage(c);return LRGuestSelectTarget(c);}
  if(flags&&( !old || (flags==LR_GPU_TEXTURE_STATE&&(p->width!=old.texture.width||p->height!=old.texture.height)) ||
     (flags==LR_GPU_TEXTURE_SUBIMAGE&&(p->x>old.texture.width||p->y>old.texture.height||p->width>old.texture.width-p->x||p->height>old.texture.height-p->y || LRTexturePixelBytes(format)!=old.storagePixelBytes || LRTextureNativeFormat(format)!=old.storagePixelFormat))))return -22;
  NSUInteger bytes=LRTextureBytes(p->width,p->height,format),pixelBytes=LRTexturePixelBytes(format);
  if(!flags&&bytes>LR_GPU_TEXTURE_BUDGET-(LRGuestTextureStorage(c)-(old?old.bytes+(old.renderTexture?old.renderTexture.width*old.renderTexture.height*4:0):0)))return -12;
  if(!LRTexturePixelBytes(format)||LRTextureNativeFormat(format)==MTLPixelFormatInvalid)return -22;
  id<MTLSamplerState> sampler=LRTextureSampler(c.device,p);if(!sampler)return -12;
  int error=LRGuestMetalFinish(c);if(error)return error;
  LRGuestMetalTexture *image=old;
  if(!flags){MTLPixelFormat pixelFormat=LRTextureNativeFormat(format);MTLTextureDescriptor *d=[MTLTextureDescriptor texture2DDescriptorWithPixelFormat:pixelFormat width:p->width height:p->height mipmapped:YES];d.storageMode=MTLStorageModeShared;d.usage=MTLTextureUsageShaderRead|(format==LR_GPU_TEXTURE_FORMAT_SRGB8_ALPHA8?0:MTLTextureUsageShaderWrite);
   id<MTLTexture> texture=[c.device newTextureWithDescriptor:d];if(!texture)return -12;
   image=old?:[LRGuestMetalTexture new];image.texture=texture;image.renderTexture=nil;image.painted=NO;image.bytes=bytes;image.storagePixelBytes=LRTexturePixelBytes(format);image.storagePixelFormat=pixelFormat;
  }
  // Pending readers completed before storage mutation.
  if(flags!=LR_GPU_TEXTURE_STATE){[image.texture replaceRegion:MTLRegionMake2D(p->x,p->y,p->width,p->height) mipmapLevel:0 withBytes:p->pixels bytesPerRow:p->width*pixelBytes];image.mipmaps=NO;image.painted=YES;image.renderTexture=nil;c.textureUploadBytes+=(uint64_t)p->width*p->height*pixelBytes;}
  image.sampler=sampler;image.minFilter=p->min_filter;image.wrapS=p->wrap_s;image.wrapT=p->wrap_t;
  if(!flags){image.colorFormat=format;c.textureBytes=LRGuestTextureStorage(c);c.textureAllocations++;c.textures[@(p->name)]=image;}
  return LRGuestSelectTarget(c);
 }
}
static int LRGuestMetalMipmap(void *context,unsigned name){
 @autoreleasepool {LRGuestMetalContext *c=(__bridge LRGuestMetalContext*)context;LRGuestMetalTexture *image=c.textures[@(name)];if(!image)return -22;
  int error=LRGuestMetalFinish(c);if(error)return error;
  if(image.texture.mipmapLevelCount==1){image.mipmaps=YES;return 0;}
  id<MTLCommandBuffer> command=[c.queue commandBuffer];id<MTLBlitCommandEncoder> blit=[command blitCommandEncoder];if(!blit)return -5;
  [blit generateMipmapsForTexture:image.texture];[blit endEncoding];[command commit];[command waitUntilCompleted];
  if(command.status!=MTLCommandBufferStatusCompleted)return -5;image.mipmaps=YES;return 0;
 }
}
static int LRGuestMetalResourceInfo(void *context,struct lr_gpu_request *r){
 LRGuestMetalContext *c=(__bridge LRGuestMetalContext*)context;
 r->count=(uint32_t)c.vertexAllocations;r->bytes=c.vertexUploadBytes;r->submissions=c.textureAllocations;r->readbacks=c.textureUploadBytes;r->gpu_nanoseconds=LRGuestTextureStorage(c);return 0;
}
static int LRGuestMetalDelete(void *context,unsigned operation,unsigned name) {
 @autoreleasepool {LRGuestMetalContext *c=(__bridge LRGuestMetalContext *)context;
  if(operation==LR_GPU_DELETE_SHADER){NSValue *v=c.shaders[@(name)];if(v)lr_glsl_shader_destroy(v.pointerValue);[c.shaders removeObjectForKey:@(name)];}
  else if(operation==LR_GPU_DELETE_TEXTURE){int error=LRGuestMetalFinish(c);if(error)return error;
   LRGuestMetalFramebuffer *f=c.framebuffers[@(c.boundFramebuffer)];if(f.textureName==name&&f.color==c.textures[@(name)]){f.color=nil;f.textureName=0;}
   [c.textures removeObjectForKey:@(name)];c.textureBytes=LRGuestTextureStorage(c);return LRGuestSelectTarget(c);}
  else {[c.programs removeObjectForKey:@(name)];[c.uniformBlockData removeObjectForKey:@(name)];}return 0;
 }
}
static int LRGuestMetalQueryBlocks(void *context,struct lr_gl_blocks *r){
 LRGuestMetalContext *c=(__bridge LRGuestMetalContext *)context;LRGuestMetalProgram *p=c.programs[@(r->program)];if(!p)return -22;*r=*(const struct lr_gl_blocks *)p.blocksData.bytes;return 0;
}
static int LRGuestMetalUniformBlocks(void *context,const struct lr_gl_block_data *r,size_t bytes){
 @autoreleasepool {LRGuestMetalContext *c=(__bridge LRGuestMetalContext *)context;LRGuestMetalProgram *p=c.programs[@(r->program)];
  const struct lr_gl_blocks *reflection=p.blocksData.bytes;
  if(!p||r->count!=reflection->count||bytes<sizeof(*r))return -22;
  for(unsigned i=0;i<r->count;i++)if(r->block[i].bytes!=reflection->block[i].bytes||r->block[i].offset<sizeof(*r)||r->block[i].offset>bytes||r->block[i].bytes>bytes-r->block[i].offset)return -22;
  NSArray<NSMutableData *> *data=c.uniformBlockData[@(r->program)];
  if(!data){NSMutableArray *fresh=[NSMutableArray new];for(unsigned i=0;i<r->count;i++)[fresh addObject:[NSMutableData dataWithLength:r->block[i].bytes]];data=fresh;c.uniformBlockData[@(r->program)]=data;}
  for(unsigned i=0;i<r->count;i++)memcpy(data[i].mutableBytes,(const char *)r+r->block[i].offset,r->block[i].bytes);
  c.blockUploadCalls++;for(unsigned i=0;i<r->count;i++)c.blockUploadBytes+=r->block[i].bytes;return 0;
 }
}
static int LRGuestMetalUniformExtents(void *context,struct lr_gl_uniform_extents *r){LRGuestMetalContext *c=(__bridge LRGuestMetalContext *)context;LRGuestMetalProgram *p=c.programs[@(r->program)];if(!p)return -22;*r=p.extents;return 0;}
static int LRGuestMetalUniformArray(void *context,const struct lr_gl_uniform_array_data *r,size_t bytes){
 @autoreleasepool {LRGuestMetalContext *c=(__bridge LRGuestMetalContext *)context;LRGuestMetalProgram *p=c.programs[@(r->program)];if(!p)return -22;
  struct lr_gl_uniform_extents extents=p.extents;struct lr_gl_link reflection=p.reflection;
  if(r->index>=extents.count)return -22;struct lr_gl_uniform_extent extent=extents.uniform[r->index];unsigned type=reflection.uniform[r->index].type,n=lr_gl_type_components(type),kind=lr_gl_type_kind(type);
  if(extent.elements<2||kind==4||!r->count||r->first>=extent.elements||r->count>extent.elements-r->first||bytes!=sizeof(*r)+(size_t)r->count*n*4)return -22;
  // Non-finite floats are valid GL uniform values (apps leave struct padding
  // uninitialised); shaders see them as GLSL would. Nothing here indexes by them.
  NSMutableData *data=p.arrayData[@(r->index)];if(data.length!=extent.elements*extent.stride)return -22;
  for(unsigned i=0;i<r->count;i++){unsigned char *target=(unsigned char *)data.mutableBytes+(r->first+i)*extent.stride;
   if(kind==3)for(unsigned j=0;j<n;j++)target[j]=!!r->data[i*n+j];else memcpy(target,r->data+i*n,n*4);
  }return 0;
 }
}
static int LRGuestMetalQueryStorages(void *context,struct lr_gl_storages *r){LRGuestMetalContext *c=(__bridge LRGuestMetalContext *)context;LRGuestMetalProgram *p=c.programs[@(r->program)];if(!p)return -22;*r=p.storages;return 0;}
static int LRGuestMetalStorageData(void *context,const struct lr_gl_storage_data *r,size_t bytes){
 @autoreleasepool {LRGuestMetalContext *c=(__bridge LRGuestMetalContext *)context;LRGuestMetalProgram *p=c.programs[@(r->program)];if(!p)return -22;struct lr_gl_storages storages=p.storages;if(r->count!=storages.count)return -22;
  NSUInteger budget=0;for(LRGuestMetalProgram *other in c.programs.allValues)if(other!=p)for(id<MTLBuffer> b in other.storageBuffers.allValues)budget+=b.length;
  for(unsigned i=0;i<r->count;i++){const struct lr_gl_storage *s=&storages.block[i];if(r->block[i].bytes<s->min_bytes||r->block[i].bytes>LR_GL_MAX_STORAGE_BYTES||r->block[i].offset<sizeof(*r)||r->block[i].offset>bytes||r->block[i].bytes>bytes-r->block[i].offset||(s->runtime_stride&&(r->block[i].bytes-s->runtime_offset)%s->runtime_stride))return -22;budget+=r->block[i].bytes;}
  if(budget>8u*1024u*1024u)return -12;
  // Reuse dedicated buffers after completing earlier readers. Unlike an arena
  // slice, a shader's unsized array cannot access adjacent draw/UBO snapshots.
  int error=LRGuestMetalFinish(c);if(error)return error;NSMutableDictionary *buffers=[NSMutableDictionary new];
  for(unsigned i=0;i<r->count;i++){id<MTLBuffer> b=p.storageBuffers[@(i)];if(b.length!=r->block[i].bytes)b=[c.device newBufferWithLength:r->block[i].bytes options:MTLResourceStorageModeShared];if(!b)return -12;buffers[@(i)]=b;}
  for(unsigned i=0;i<r->count;i++){id<MTLBuffer> b=buffers[@(i)];memcpy(b.contents,(const char *)r+r->block[i].offset,r->block[i].bytes);}
  p.storageBuffers=buffers;return 0;
 }
}
static int LRGuestMetalInstanceAttributes(void *context,const struct lr_gpu_instance_attributes *p){LRGuestMetalContext *c=(__bridge LRGuestMetalContext *)context;if(!c.instanceData)c.instanceData=[NSMutableData dataWithLength:sizeof *p];memcpy(c.instanceData.mutableBytes,p,sizeof *p);return 0;}
static NSArray *LRGuestInstancePipeline(LRGuestMetalContext *c,LRGuestMetalProgram *p){
 const struct lr_gpu_instance_attributes *data=c.instanceData.bytes;BOOL used=NO;NSMutableString *key=[NSMutableString stringWithFormat:@"%p",(__bridge void*)p.pipeline];
 if(data)for(unsigned a=0;a<LR_GL_MAX_ATTRIBUTES;a++){[key appendFormat:@":%u",data->divisor[a]];if(data->divisor[a])used=YES;}
 if(!used)return @[p.pipeline,p.descriptor];
 NSArray *entry=c.instancePipelines[key];if(entry)return @[entry[1],entry[2]];
 MTLRenderPipelineDescriptor *descriptor=[p.descriptor copy];MTLVertexDescriptor *v=[descriptor.vertexDescriptor copy];
 for(unsigned a=0;a<p.reflection.attributes;a++){unsigned columns=p.reflection.attribute[a].type==0x8b5c?4:1;for(unsigned col=0;col<columns;col++){unsigned location=p.reflection.attribute[a].location+col;
  // Instance buffers occupy vertex indices 21..28 (divisor[] has LR_GL_MAX_ATTRIBUTES
  // entries; 29/30 are reserved). Matrix columns past that range cannot be encoded:
  // refuse the draw (the bridge falls back) rather than build an invalid descriptor.
  if(location>=LR_GL_MAX_ATTRIBUTES)return nil;
  if(data->divisor[location]){v.attributes[location].bufferIndex=21+location;v.attributes[location].offset=0;v.layouts[21+location].stride=16;v.layouts[21+location].stepFunction=MTLVertexStepFunctionPerInstance;v.layouts[21+location].stepRate=data->divisor[location];}}}
 // A layout no attribute reads any more (every attribute became per-instance,
 // e.g. glyph batches) must not stay described: Metal rejects the descriptor.
 {BOOL referenced[31]={NO};for(unsigned i=0;i<31;i++){MTLVertexAttributeDescriptor *at=v.attributes[i];if(at.format!=MTLVertexFormatInvalid&&at.bufferIndex<31)referenced[at.bufferIndex]=YES;}
  for(unsigned i=0;i<31;i++)if(!referenced[i]&&v.layouts[i].stride){v.layouts[i].stride=0;v.layouts[i].stepFunction=MTLVertexStepFunctionPerVertex;v.layouts[i].stepRate=1;}}
 // Metal copies vertexDescriptor on assignment; publish after all mutations.
 descriptor.vertexDescriptor=v;
 if(getenv("LR_GUEST_METAL_TRACE_PIPELINES")){int fd=fcntl(666,F_GETFD)!=-1?666:2;
  for(unsigned i=0;i<31;i++){MTLVertexAttributeDescriptor *at=v.attributes[i];if(at.format!=MTLVertexFormatInvalid)dprintf(fd,"GUEST_METAL_VERTEX attribute=%u format=%lu buffer=%lu offset=%lu\n",i,(unsigned long)at.format,(unsigned long)at.bufferIndex,(unsigned long)at.offset);}
  for(unsigned i=0;i<31;i++){MTLVertexBufferLayoutDescriptor *l=v.layouts[i];if(l.stride)dprintf(fd,"GUEST_METAL_VERTEX layout=%u stride=%lu step=%lu rate=%lu\n",i,(unsigned long)l.stride,(unsigned long)l.stepFunction,(unsigned long)l.stepRate);}
  for(unsigned a=0;a<p.reflection.attributes;a++)dprintf(fd,"GUEST_METAL_VERTEX reflected=%s location=%d type=%x\n",p.reflection.attribute[a].name,(int)p.reflection.attribute[a].location,p.reflection.attribute[a].type);
  for(unsigned a=0;a<LR_GL_MAX_ATTRIBUTES;a++)if(data->divisor[a])dprintf(fd,"GUEST_METAL_VERTEX divisor[%u]=%u\n",a,data->divisor[a]);}
 NSError *error=nil;id<MTLRenderPipelineState> pipeline=[c.device newRenderPipelineStateWithDescriptor:descriptor error:&error];if(!pipeline){fprintf(stderr,"GUEST_METAL_INSTANCE_PIPELINE_ERROR %s\n",error.localizedDescription.UTF8String);return nil;}
 if(!c.instancePipelines)c.instancePipelines=[NSMutableDictionary new];if(c.instancePipelines.count>=64)[c.instancePipelines removeAllObjects];c.instancePipelines[key]=@[p.pipeline,pipeline,descriptor];return @[pipeline,descriptor];
}
static int LRGuestMetalDrawProgramIndexed(void *context,const struct lr_gl_draw *r,unsigned flags,unsigned instances,const uint32_t *indices,unsigned indexCount,unsigned baseInstance) {
 @autoreleasepool {
  LRGuestMetalContext *c=(__bridge LRGuestMetalContext *)context;LRGuestMetalProgram *p=c.programs[@(r->program)];
  if(!instances||instances>LR_GPU_MAX_INSTANCES||!p||!c.target||(c.boundFramebuffer&&LRGuestFramebufferStatus(c.framebuffers[@(c.boundFramebuffer)])!=0x8cd5))return -22;
  // Typed packets are opaque to the syscall layer. Validate the values using
  // immutable compiler reflection before any cast, encoding or resource lookup.
  const struct lr_gpu_instance_attributes *typedInstances=c.instanceData.bytes;
  struct lr_gl_link reflection=p.reflection;struct lr_gl_uniform_extents extents=p.extents;struct lr_gl_storages storages=p.storages;
  uint32_t storageSizes[31]={0};for(unsigned i=0;i<storages.count;i++){id<MTLBuffer> buffer=p.storageBuffers[@(i)];if(buffer.length<storages.block[i].min_bytes)return -22;
   if(storages.block[i].vertex_binding!=UINT32_MAX){storageSizes[storages.block[i].vertex_binding]=buffer.length;if(typedInstances)for(unsigned a=0;a<LR_GL_MAX_ATTRIBUTES;a++)if(typedInstances->divisor[a])return -22;}
   if(storages.block[i].fragment_binding!=UINT32_MAX)storageSizes[storages.block[i].fragment_binding]=buffer.length;
  }
  for(unsigned i=0;i<reflection.uniforms;i++){
   unsigned kind=lr_gl_type_kind(reflection.uniform[i].type),n=lr_gl_type_components(reflection.uniform[i].type);
   if(kind==4)for(unsigned j=0;j<n;j++)if(!isfinite(r->uniform[i][j]))return -22;   // sampler names index tables; float values may be non-finite as in GL
   if(kind==4&&(r->uniform[i][0]<0||(r->uniform[i][0]>LR_GPU_MAX_TEXTURES&&(r->uniform[i][0]<=LR_GPU_RESIDENT_SAMPLER_BASE||r->uniform[i][0]>LR_GPU_RESIDENT_SAMPLER_BASE+LR_GPU_MAX_RESIDENT))||r->uniform[i][0]!=floorf(r->uniform[i][0])))return -22;
  }
  for(unsigned a=0;a<reflection.attributes;a++){
   unsigned columns=reflection.attribute[a].type==0x8b5c?4:1;for(unsigned col=0;col<columns;col++){unsigned slot=reflection.attribute[a].location+col,kind=lr_gl_type_kind(reflection.attribute[a].type),n=columns==4?4:lr_gl_type_components(reflection.attribute[a].type);
   if(kind)continue;
   if(typedInstances&&typedInstances->divisor[slot]){for(unsigned i=0;i<typedInstances->count[slot];i++)for(unsigned j=0;j<n;j++)if(!isfinite(typedInstances->value[slot][i][j]))return -22;}
   else for(unsigned i=0;i<r->count;i++)for(unsigned j=0;j<n;j++)if(!isfinite(r->vertex[i].attribute[slot][j]))return -22;
  }}
  if(c.boundFramebuffer&&!c.framebuffers[@(c.boundFramebuffer)].depth)flags&=~LR_GPU_DEPTH_TEST_BIT;
  for(unsigned i=0;i<p.reflection.uniforms;i++)if(p.reflection.uniform[i].type==0x8b5e&&c.boundFramebuffer&&c.textures[@((unsigned)r->uniform[i][0])]==c.framebuffers[@(c.boundFramebuffer)].color)return -22;
  const struct lr_gl_blocks *reflectionBlocks=p.blocksData.bytes;
  NSArray<NSMutableData *> *blocks=c.uniformBlockData[@(r->program)];if(reflectionBlocks->count&&blocks.count!=reflectionBlocks->count)return -22;
  size_t blockBytes=0;for(unsigned i=0;i<reflectionBlocks->count;i++){if(blocks[i].length!=reflectionBlocks->block[i].bytes)return -22;blockBytes+=(blocks[i].length+255)&~(NSUInteger)255;}
  size_t arrayBytes=0;for(unsigned i=0;i<extents.count;i++)if(extents.uniform[i].elements>1){if(p.arrayData[@(i)].length!=extents.uniform[i].elements*extents.uniform[i].stride)return -22;arrayBytes+=(p.arrayData[@(i)].length+255)&~(NSUInteger)255;}
  blockBytes+=arrayBytes;
  size_t bytes=(size_t)r->count*sizeof(struct lr_gl_vertex),indexBytes=(size_t)indexCount*sizeof(uint32_t);
  if(((bytes+255)&~(NSUInteger)255)+blockBytes+((indexBytes+255)&~(NSUInteger)255)>1048576)return -12;
  if(c.pendingBytes+bytes+indexBytes+sizeof r->uniform+blockBytes>1048576||((c.vertexOffset+255)&~(NSUInteger)255)+((bytes+255)&~(NSUInteger)255)+blockBytes+((indexBytes+255)&~(NSUInteger)255)>1048576){int e=LRGuestMetalSubmit(c);if(e)return e;}
  if((blockBytes||indexBytes)&&!c.vertexArena){c.vertexArena=[c.device newBufferWithLength:1048576 options:MTLResourceStorageModeShared];if(!c.vertexArena)return -12;c.vertexAllocations++;}
  if(!c.pending){c.pending=[c.queue commandBuffer];if(!c.pending)return -5;}
  if(!c.encoder){MTLRenderPassDescriptor *d=[MTLRenderPassDescriptor renderPassDescriptor];
    d.colorAttachments[0].texture=c.target;d.colorAttachments[0].loadAction=c.painted?MTLLoadActionLoad:MTLLoadActionClear;d.colorAttachments[0].clearColor=MTLClearColorMake(0,0,0,0);d.colorAttachments[0].storeAction=MTLStoreActionStore;
    for(unsigned i=1;i<c.mrtColors.count&&!c.boundFramebuffer;i++){d.colorAttachments[i].texture=c.mrtColors[i];d.colorAttachments[i].loadAction=MTLLoadActionLoad;d.colorAttachments[i].storeAction=MTLStoreActionStore;}
    d.depthAttachment.texture=c.depth;d.depthAttachment.loadAction=c.painted?MTLLoadActionLoad:MTLLoadActionClear;d.depthAttachment.clearDepth=1;d.depthAttachment.storeAction=MTLStoreActionStore;d.stencilAttachment.texture=c.stencil;d.stencilAttachment.loadAction=(c.residentBound||(c.boundFramebuffer?c.offscreenStencilPainted:c.defaultStencilPainted))?MTLLoadActionLoad:MTLLoadActionClear;d.stencilAttachment.clearStencil=0;d.stencilAttachment.storeAction=MTLStoreActionStore;
    c.encoder=[c.pending renderCommandEncoderWithDescriptor:d];if(!c.encoder)return -5;
  }
  id<MTLRenderCommandEncoder> e=c.encoder;float frameSize[4]={(float)c.target.width,(float)c.target.height,c.rasterState.depth_bits==24?16777215.0f:c.rasterState.depth_bits==16?65535.0f:0,0};[e setFragmentBytes:frameSize length:sizeof frameSize atIndex:30];NSArray *instanced=LRGuestInstancePipeline(c,p);if(!instanced)return -5;id<MTLRenderPipelineState> pipeline=LRGuestBlendPipeline(c,instanced[0],instanced[1]);if(!pipeline)return -5;[e setRenderPipelineState:pipeline];LRGuestSetBlendColor(c,e);
  id<MTLDepthStencilState> depth=LRGuestDepthPipeline(c,flags);if(!depth)return -12;[e setDepthStencilState:depth];[e setStencilFrontReferenceValue:c.stencilState.front.reference backReferenceValue:c.stencilState.back.reference];if(c.boundFramebuffer)c.offscreenStencilPainted=YES;else c.defaultStencilPainted=YES;BOOL raster=LRGuestRaster(c,e,flags);
  int uploadError=LRGuestMetalVertices(c,e,r->vertex,bytes);if(uploadError)return uploadError;
  if(storages.count){[e setVertexBytes:storageSizes length:sizeof storageSizes atIndex:29];[e setFragmentBytes:storageSizes length:sizeof storageSizes atIndex:29];}
  for(unsigned i=0;i<storages.count;i++){id<MTLBuffer> b=p.storageBuffers[@(i)];if(storages.block[i].vertex_binding!=UINT32_MAX)[e setVertexBuffer:b offset:0 atIndex:storages.block[i].vertex_binding];if(storages.block[i].fragment_binding!=UINT32_MAX)[e setFragmentBuffer:b offset:0 atIndex:storages.block[i].fragment_binding];}
  const struct lr_gpu_instance_attributes *instanceData=c.instanceData.bytes;
  if(instanceData)for(unsigned a=0;a<LR_GL_MAX_ATTRIBUTES;a++)if(instanceData->divisor[a]){if(instanceData->instances!=instances+baseInstance)return -22;[e setVertexBytes:instanceData->value[a] length:instanceData->count[a]*16 atIndex:21+a];}
  // Immutable per-draw snapshots in the existing shared upload arena. Later
  // BufferSubData calls cannot overwrite data referenced by pending GPU work.
  for(unsigned i=0;i<reflectionBlocks->count;i++){
   NSUInteger offset=(c.vertexOffset+255)&~(NSUInteger)255;memcpy((char *)c.vertexArena.contents+offset,blocks[i].bytes,blocks[i].length);c.vertexOffset=offset+blocks[i].length;c.blockSnapshotBytes+=blocks[i].length;
   const struct lr_gl_block *block=&reflectionBlocks->block[i];if(block->vertex_binding!=UINT32_MAX)[e setVertexBuffer:c.vertexArena offset:offset atIndex:block->vertex_binding];if(block->fragment_binding!=UINT32_MAX)[e setFragmentBuffer:c.vertexArena offset:offset atIndex:block->fragment_binding];
  }
  for(unsigned i=0;i<reflection.uniforms;i++){unsigned v=reflection.uniform[i].vertex_binding,f=reflection.uniform[i].fragment_binding;
    if(extents.uniform[i].elements>1){NSMutableData *array=p.arrayData[@(i)];NSUInteger offset=(c.vertexOffset+255)&~(NSUInteger)255;memcpy((char *)c.vertexArena.contents+offset,array.bytes,array.length);c.vertexOffset=offset+array.length;
     if(v!=UINT32_MAX)[e setVertexBuffer:c.vertexArena offset:offset atIndex:v];if(f!=UINT32_MAX)[e setFragmentBuffer:c.vertexArena offset:offset atIndex:f];
    }else if(reflection.uniform[i].type==0x8b5e){float requested=r->uniform[i][0];if(requested<0||(requested>LR_GPU_MAX_TEXTURES&&(requested<=LR_GPU_RESIDENT_SAMPLER_BASE||requested>LR_GPU_RESIDENT_SAMPLER_BASE+LR_GPU_MAX_RESIDENT))||requested!=floorf(requested))return -22;
      LRGuestMetalTexture *t=c.textures[@((unsigned)requested)];BOOL complete=t.texture!=nil;
      BOOL pot=t&&!(t.texture.width&(t.texture.width-1))&&!(t.texture.height&(t.texture.height-1));
      if(t&&t.minFilter>=0x2700&&((t.texture.width>1||t.texture.height>1)&&!t.mipmaps))complete=NO;
      if(t&&!pot&&(t.minFilter>=0x2700||t.wrapS!=0x812f||t.wrapT!=0x812f))complete=NO;
      if(v!=UINT32_MAX){[e setVertexTexture:complete?t.texture:c.incompleteTexture atIndex:v];[e setVertexSamplerState:complete?t.sampler:c.incompleteSampler atIndex:v];}
      if(f!=UINT32_MAX){[e setFragmentTexture:complete?t.texture:c.incompleteTexture atIndex:f];[e setFragmentSamplerState:complete?t.sampler:c.incompleteSampler atIndex:f];}
    }else{const void *data=r->uniform[i];unsigned char boolean[64]={0};
      if(lr_gl_type_kind(reflection.uniform[i].type)==3){for(unsigned j=0;j<lr_gl_type_components(reflection.uniform[i].type);j++){uint32_t b;memcpy(&b,r->uniform[i]+j,4);boolean[j]=!!b;}data=boolean;}
      float columns[16]={0};if(reflection.uniform[i].type==0x8b5b){for(unsigned col=0;col<3;col++)memcpy(columns+col*4,r->uniform[i]+col*3,12);data=columns;}   // GL mat3 is 9 packed floats; MSL float3x3 columns are 16 bytes
      if(v!=UINT32_MAX)[e setVertexBytes:data length:64 atIndex:v];if(f!=UINT32_MAX)[e setFragmentBytes:data length:64 atIndex:f];}
  }
  if(indices){NSUInteger offset=(c.vertexOffset+255)&~(NSUInteger)255;memcpy((char *)c.vertexArena.contents+offset,indices,indexBytes);c.vertexOffset=offset+indexBytes;
    if(raster&&LRGuestMetalSetViewport(c,e))[e drawIndexedPrimitives:(flags&LR_GPU_POINTS_BIT)?MTLPrimitiveTypePoint:(flags&LR_GPU_LINES_BIT)?MTLPrimitiveTypeLine:MTLPrimitiveTypeTriangle indexCount:indexCount indexType:MTLIndexTypeUInt32 indexBuffer:c.vertexArena indexBufferOffset:offset instanceCount:instances baseVertex:0 baseInstance:baseInstance];
  }else if(raster&&LRGuestMetalSetViewport(c,e))[e drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:r->count instanceCount:instances baseInstance:baseInstance];c.painted=YES;c.pendingDraws++;c.pendingBytes+=bytes+indexBytes+sizeof r->uniform+blockBytes;
  return c.pendingDraws>=64?LRGuestMetalSubmit(c):0;
 }
}
static int LRGuestMetalDrawProgramInstances(void *context,const struct lr_gl_draw *r,unsigned flags,unsigned instances){return LRGuestMetalDrawProgramIndexed(context,r,flags,instances,NULL,0,0);}
static int LRGuestMetalIndexed(void *context,const struct lr_gl_draw *r,const uint32_t *indices,unsigned count,unsigned flags,unsigned instances){return LRGuestMetalDrawProgramIndexed(context,r,flags,instances,indices,count,0);}
static int LRGuestMetalIndexedBase(void *context,const struct lr_gl_draw *r,const uint32_t *indices,unsigned count,unsigned flags,unsigned instances,unsigned base){return LRGuestMetalDrawProgramIndexed(context,r,flags,instances,indices,count,base);}
static int LRGuestMetalDrawProgram(void *context,const struct lr_gl_draw *r,unsigned flags){return LRGuestMetalDrawProgramInstances(context,r,flags,1);}
static int LRGuestMetalResize(void *context,unsigned w,unsigned h){
 @autoreleasepool { LRGuestMetalContext *c=(__bridge LRGuestMetalContext *)context;
  int e=LRGuestMetalFinish(c);if(e)return e;
  MTLTextureDescriptor *d=[MTLTextureDescriptor texture2DDescriptorWithPixelFormat:c.defaultTarget.pixelFormat width:w height:h mipmapped:NO];d.storageMode=MTLStorageModeShared;d.usage=MTLTextureUsageRenderTarget|MTLTextureUsagePixelFormatView;
  id<MTLTexture> color=[c.device newTextureWithDescriptor:d];d.pixelFormat=MTLPixelFormatDepth32Float;d.storageMode=MTLStorageModePrivate;d.usage=MTLTextureUsageRenderTarget;id<MTLTexture> depth=[c.device newTextureWithDescriptor:d];
  d.pixelFormat=MTLPixelFormatStencil8;id<MTLTexture> stencil=[c.device newTextureWithDescriptor:d];if(!color||!depth||!stencil)return -12;c.defaultStencil=stencil;c.defaultStencilPainted=NO;c.defaultTarget=color;c.mrtColors=nil;c.mrtMasks=nil;c.defaultSrgbView=nil;c.defaultDepth=depth;c.defaultPainted=NO;return LRGuestSelectTarget(c);
 }
}
static int LRGuestMetalFramebufferCommand(void *context,struct lr_gpu_framebuffer *r){
 @autoreleasepool {LRGuestMetalContext *c=(__bridge LRGuestMetalContext*)context;
  unsigned previous=c.boundFramebuffer;
  int error=0;if(r->command!=LR_GPU_FB_QUERY){error=LRGuestMetalFinish(c);if(error)return error;}
  if(r->command==LR_GPU_FB_BIND){
   if(r->name&&!c.framebuffers[@(r->name)])c.framebuffers[@(r->name)]=[LRGuestMetalFramebuffer new];
   c.boundFramebuffer=r->name;
  }else if(r->command==LR_GPU_FB_ATTACH){
   LRGuestMetalFramebuffer *f=c.framebuffers[@(r->name)];if(!f||r->name!=c.boundFramebuffer)return -22;
   LRGuestMetalTexture *t=nil;if(r->texture){t=c.textures[@(r->texture)];if(!t){t=[LRGuestMetalTexture new];c.textures[@(r->texture)]=t;}}
   f.color=t;f.textureName=r->texture;f.format=r->format;
  }else if(r->command==LR_GPU_FB_DEPTH_RENDERBUFFER){
   LRGuestMetalFramebuffer *f=c.framebuffers[@(r->name)];if(!f||r->name!=c.boundFramebuffer)return -22;
   LRGuestMetalRenderbuffer *depth=r->texture?c.renderbuffers[@(r->texture)]:nil;if(r->texture&&!depth)return -22;
   f.depth=depth;f.depthName=r->texture;
  }else if(r->command==LR_GPU_FB_DELETE){
   if(c.boundFramebuffer==r->name)c.boundFramebuffer=0;
   [c.framebuffers removeObjectForKey:@(r->name)];
  }else if(r->command!=LR_GPU_FB_QUERY)return -22;
  if(r->command!=LR_GPU_FB_QUERY){error=LRGuestSelectTarget(c);if(error){if(r->command==LR_GPU_FB_BIND){c.boundFramebuffer=previous;LRGuestSelectTarget(c);}return error;}}
  LRGuestMetalFramebuffer *f=c.framebuffers[@(c.boundFramebuffer)];
  r->status=c.boundFramebuffer?LRGuestFramebufferStatus(f):0x8cd5;
  r->width=(unsigned)c.target.width;r->height=(unsigned)c.target.height;if(r->command==LR_GPU_FB_QUERY&&r->format==0x8d00){r->texture=f.depthName;r->format=f.depth?0x8d41:0;}else r->texture=f.textureName;
  return 0;
 }
}
static NSUInteger LRGuestDepthStorage(LRGuestMetalContext *c){NSMutableSet *images=[NSMutableSet setWithArray:c.renderbuffers.allValues];for(LRGuestMetalFramebuffer *f in c.framebuffers.allValues)if(f.depth)[images addObject:f.depth];NSUInteger bytes=0;for(LRGuestMetalRenderbuffer *r in images)bytes+=(NSUInteger)r.texture.width*r.texture.height*4;return bytes;}
static int LRGuestMetalRenderbufferCommand(void *context,struct lr_gpu_renderbuffer *r){
 @autoreleasepool {LRGuestMetalContext *c=(__bridge LRGuestMetalContext *)context;LRGuestMetalRenderbuffer *b=c.renderbuffers[@(r->name)];
  if(r->command==LR_GPU_RB_BIND){if(!b){b=[LRGuestMetalRenderbuffer new];c.renderbuffers[@(r->name)]=b;}}
  else if(!b)return -22;
  else if(r->command==LR_GPU_RB_STORAGE){
   NSUInteger bytes=(NSUInteger)r->width*r->height*4,old=(NSUInteger)b.texture.width*b.texture.height*4;if(bytes>LR_GPU_RENDERBUFFER_BUDGET-(LRGuestDepthStorage(c)-old))return -12;
   int error=LRGuestMetalFinish(c);if(error)return error;
   id<MTLTexture> texture=nil;if(r->width&&r->height){MTLTextureDescriptor *d=[MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatDepth32Float width:r->width height:r->height mipmapped:NO];d.storageMode=MTLStorageModePrivate;d.usage=MTLTextureUsageRenderTarget|MTLTextureUsagePixelFormatView;texture=[c.device newTextureWithDescriptor:d];if(!texture)return -12;}
   b.texture=texture;b.width=r->width;b.height=r->height;b.format=r->format;
  }else if(r->command==LR_GPU_RB_DELETE){int error=LRGuestMetalFinish(c);if(error)return error;LRGuestMetalFramebuffer *f=c.framebuffers[@(c.boundFramebuffer)];if(f.depth==b&&f.depthName==r->name){f.depth=nil;f.depthName=0;}[c.renderbuffers removeObjectForKey:@(r->name)];}
  else if(r->command!=LR_GPU_RB_QUERY)return -22;
  r->width=b.width;r->height=b.height;r->format=b.format;r->depth_bits=b.texture?32:0;
  return r->command==LR_GPU_RB_QUERY?0:LRGuestSelectTarget(c);
 }
}
static int LRGuestMetalUniformInfo(void *context,struct lr_gpu_request *r){LRGuestMetalContext *c=(__bridge LRGuestMetalContext *)context;r->submissions=c.blockUploadCalls;r->bytes=c.blockUploadBytes;r->gpu_nanoseconds=c.blockSnapshotBytes;return 0;}
static int LRGuestMetalCopyInfo(void *context,struct lr_gpu_request *r){LRGuestMetalContext *c=(__bridge LRGuestMetalContext *)context;r->submissions=c.computeCopies;r->readbacks=c.rowCopies;r->bytes=c.copyBytes;return 0;}
#include "GuestMetalResident.h"
static void LRInstallGuestMetalBackend(void) {
    static const struct lr_gpu_backend backend={LRGuestMetalCreate,LRGuestMetalDraw,LRGuestMetalRead,LRGuestMetalInfo,LRGuestMetalDestroy,LRGuestMetalSync,LRGuestMetalCompile,LRGuestMetalLink,LRGuestMetalDelete,LRGuestMetalDrawProgram,LRGuestMetalResize,LRGuestMetalTextureUpdate,LRGuestMetalMipmap,LRGuestMetalResourceInfo,LRGuestMetalReadRegion,LRGuestMetalViewport,LRGuestMetalFramebufferCommand,LRGuestMetalQueryBlocks,LRGuestMetalUniformBlocks,LRGuestMetalBlend,LRGuestMetalDrawProgramInstances,LRGuestMetalUniformInfo,LRGuestMetalRenderbufferCommand,LRGuestMetalCopyInfo,LRGuestMetalIndexed,LRGuestMetalInstanceAttributes,LRGuestMetalUniformExtents,LRGuestMetalUniformArray,LRGuestMetalIndexedBase,LRGuestMetalQueryStorages,LRGuestMetalStorageData,LRGuestMetalDepthState,LRGuestMetalImportColor,LRGuestMetalRasterState,LRGuestMetalImportFrame,LRGuestMetalReadFrame,LRGuestMetalDepthTransfer,LRGuestMetalImportMrt,LRGuestMetalReadMrt,LRGuestMetalStencilState,LRGuestMetalStencilTransfer,LRGuestMetalResident};
    lr_gpu_install(&backend);
}
