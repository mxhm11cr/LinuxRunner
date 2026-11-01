#ifndef LR_SHADER_COMPILER_H
#define LR_SHADER_COMPILER_H
#include <stdint.h>
#define LR_GL_MAX_ATTRIBUTES 8
#define LR_GL_MAX_UNIFORMS 16
#define LR_GL_MAX_SHADERS 128   // two per program slot (the Mesa bridge names them slot*2+stage+1)
#define LR_GL_MAX_PROGRAMS 64   // per context; Blender alone uses 25-40 per redraw (8 thrashed: recompiles on most draws)
#define LR_GL_LEGACY_SOURCE 16384
#define LR_GL_MAX_SOURCE 131072
#define LR_GL_LOG_SIZE 2048
struct lr_gl_variable { char name[64]; uint32_t type, location, vertex_binding, fragment_binding; };
struct lr_gl_compile_v1 { uint32_t name,stage,length,status;char source[LR_GL_LEGACY_SOURCE];char log[LR_GL_LOG_SIZE]; };
struct lr_gl_compile { uint32_t name,stage,length,status; char source[LR_GL_MAX_SOURCE];char log[LR_GL_LOG_SIZE]; };
struct lr_gl_link {uint32_t name,vertex,fragment,status,attributes,uniforms;struct lr_gl_variable attribute[LR_GL_MAX_ATTRIBUTES],uniform[LR_GL_MAX_UNIFORMS];char log[LR_GL_LOG_SIZE];};
#define LR_GL_MAX_BLOCK_BINDINGS 16
#define LR_GL_MAX_BLOCKS 4
#define LR_GL_LEGACY_BLOCK_MEMBERS 16
#define LR_GL_MAX_BLOCK_MEMBERS 1024
#define LR_GL_MAX_BLOCK_BYTES 65536
struct lr_gl_block_member { char name[64]; uint32_t type,size,offset,array_stride,matrix_stride,row_major; };
struct lr_gl_block { char name[64];uint32_t bytes,binding,vertex_binding,fragment_binding,members;struct lr_gl_block_member member[LR_GL_MAX_BLOCK_MEMBERS]; };
struct lr_gl_blocks { uint32_t program,count;struct lr_gl_block block[LR_GL_MAX_BLOCKS]; };
// Operation 21's original layout remains usable by installed older clients.
struct lr_gl_block_v1 { char name[64];uint32_t bytes,binding,vertex_binding,fragment_binding,members;struct lr_gl_block_member member[LR_GL_LEGACY_BLOCK_MEMBERS]; };
struct lr_gl_blocks_v1 { uint32_t program,count;struct lr_gl_block_v1 block[LR_GL_MAX_BLOCKS]; };
#define LR_GL_MAX_UNIFORM_ELEMENTS 256
#define LR_GL_MAX_UNIFORM_ARRAY_BYTES 16384
struct lr_gl_uniform_extent { uint32_t elements,stride; };
struct lr_gl_uniform_extents { uint32_t program,count;struct lr_gl_uniform_extent uniform[LR_GL_MAX_UNIFORMS]; };
struct lr_gl_uniform_array_data { uint32_t program,index,first,count;uint32_t data[]; };
#define LR_GL_MAX_STORAGE_BLOCKS 8
#define LR_GL_MAX_STORAGE_BYTES 262144
struct lr_gl_storage { char name[64];uint32_t binding,vertex_binding,fragment_binding,min_bytes,runtime_offset,runtime_stride; };
struct lr_gl_storages { uint32_t program,count;struct lr_gl_storage block[LR_GL_MAX_STORAGE_BLOCKS]; };
struct lr_gl_storage_data { uint32_t program,count;struct {uint32_t offset,bytes;} block[LR_GL_MAX_STORAGE_BLOCKS];unsigned char data[]; };
// Separate packets extend v1 without changing existing link/draw layouts.
struct lr_gl_block_data { uint32_t program,count;struct {uint32_t offset,bytes;} block[LR_GL_MAX_BLOCKS];unsigned char data[]; };
struct lr_gl_vertex { float attribute[LR_GL_MAX_ATTRIBUTES][4]; };
struct lr_gl_draw { uint32_t program,count;float uniform[LR_GL_MAX_UNIFORMS][16];struct lr_gl_vertex vertex[]; };
// Reflected packet scalars preserve their 32-bit representation. Boolean
// default uniforms use GL's 32-bit storage and are packed for MSL on the host.
static inline unsigned lr_gl_type_kind(unsigned type){
 if(type==0x1404||(type>=0x8b53&&type<=0x8b55))return 1;
 if(type==0x1405||(type>=0x8dc6&&type<=0x8dc8))return 2;
 if(type>=0x8b56&&type<=0x8b59)return 3;
 if(type==0x8b5e)return 4;return 0;
}
static inline unsigned lr_gl_type_components(unsigned type){
 if(type==0x8b5c)return 16;
 if(type==0x8b5b)return 9;   // mat3: three packed vec3 columns (MSL pads each to 16 bytes)
 if(type==0x8b5a)return 4;
 if(type>=0x8b50&&type<=0x8b52)return type-0x8b50+2;
 if(type>=0x8b53&&type<=0x8b55)return type-0x8b53+2;
 if(type>=0x8dc6&&type<=0x8dc8)return type-0x8dc6+2;
 if(type>=0x8b56&&type<=0x8b59)return type-0x8b56+1;
 return 1;
}
#ifdef __cplusplus
extern "C" {
#endif
void *lr_glsl_compile(unsigned stage,const char *source,char *log);
void lr_glsl_shader_destroy(void *shader);
void *lr_glsl_link(void *vertex,void *fragment,struct lr_gl_link *reflection);
void lr_glsl_blocks(void *program,struct lr_gl_blocks *reflection);
void lr_glsl_uniform_extents(void *program,struct lr_gl_uniform_extents *reflection);
void lr_glsl_storages(void *program,struct lr_gl_storages *reflection);
const char *lr_glsl_msl(void *program,unsigned stage);
void lr_glsl_program_destroy(void *program);
#ifdef __cplusplus
}
#endif
#endif
