// GLSL ES 1.00 reference parsing -> SPIR-V -> MSL. No source rewriting or
// application-name matching. Bounded subset reflection rejects unsupported state.
#include "shader_compiler.h"
#include <glslang/Public/ShaderLang.h>
#include <glslang/Public/ResourceLimits.h>
#include <glslang/MachineIndependent/localintermediate.h>
#include <SPIRV/GlslangToSpv.h>
#include <spirv_msl.hpp>
#include <memory>
#include <mutex>
#include <string>
#include <cstring>
#include <stdexcept>
#include <algorithm>
#include <map>
#include <functional>
#include <regex>
static void validate_block_types(const glslang::TType &t,unsigned depth=0){
 if(depth>4)throw std::runtime_error("Uniform block nesting exceeds limit");
 if(t.getBasicType()==glslang::EbtBlock||t.getBasicType()==glslang::EbtStruct)for(const auto &member:*t.getStruct())validate_block_types(*member.type,depth+1);
}
// glslang lowers std140 booleans to 32-bit unsigned SPIR-V storage.
// Retain their GL-facing types from the checked GLSL AST; Metal consumes the
// original 32-bit values and performs != 0, including high-byte-only truth.
static void blockBooleanTypes(const glslang::TType &t,const std::string &path,std::map<std::string,unsigned> &types){
 if(t.getBasicType()==glslang::EbtBool)types[path]=0x8b56+t.getVectorSize()-1;
 else if(t.getBasicType()==glslang::EbtBlock||t.getBasicType()==glslang::EbtStruct)for(const auto &member:*t.getStruct())blockBooleanTypes(*member.type,path+"."+member.type->getFieldName().c_str(),types);
}
static std::string blockTypePath(const std::string &path){std::string out;bool index=false;for(char c:path){if(c=='[')index=true;else if(c==']')index=false;else if(!index)out+=c;}return out;}
struct BlockQualifiers : glslang::TIntermTraverser {
 std::map<std::string,unsigned> bindings,storageBindings;std::map<std::string,bool> instances;std::map<std::string,unsigned> booleanTypes;
 BlockQualifiers():TIntermTraverser(true,false,false){}
 void visitSymbol(glslang::TIntermSymbol *s) override {
  const auto &t=s->getType();const auto &q=t.getQualifier();
  if(t.getBasicType()==glslang::EbtBlock&&q.storage==glslang::EvqBuffer)storageBindings[t.getTypeName().c_str()]=q.hasBinding()?q.layoutBinding:0;
  if(t.getBasicType()==glslang::EbtBlock&&q.storage==glslang::EvqUniform){
   if(q.layoutPacking!=glslang::ElpStd140)throw std::runtime_error("Uniform blocks require explicit std140 layout");
   validate_block_types(t);blockBooleanTypes(t,t.getTypeName().c_str(),booleanTypes);bindings[t.getTypeName().c_str()]=q.hasBinding()?q.layoutBinding:0;instances[t.getTypeName().c_str()]=s->getName().find(glslang::AnonymousPrefix)!=0;
  }
 }
};
struct Shader { std::string source;std::unique_ptr<glslang::TShader> parsed;EShLanguage stage;std::map<std::string,unsigned> blockBindings,storageBindings;std::map<std::string,bool> blockInstances;std::map<std::string,unsigned> blockBooleans; };
struct Program {std::string msl[2];lr_gl_blocks blocks{};lr_gl_uniform_extents extents{};lr_gl_storages storages{};};
static void error(char *log,const char *message){std::strncpy(log,message,LR_GL_LOG_SIZE-1);log[LR_GL_LOG_SIZE-1]=0;}
static unsigned gltype(const spirv_cross::SPIRType &t){
 if(!t.array.empty()||(t.basetype!=spirv_cross::SPIRType::Boolean&&t.width!=32))throw std::runtime_error("Uniform/attribute arrays or non-32-bit types unavailable");
 if(t.basetype==spirv_cross::SPIRType::Int||t.basetype==spirv_cross::SPIRType::UInt||t.basetype==spirv_cross::SPIRType::Boolean){
  if(t.columns!=1||t.vecsize<1||t.vecsize>4)throw std::runtime_error("Unsupported integer/boolean shape");
  if(t.basetype==spirv_cross::SPIRType::Int)return t.vecsize==1?0x1404:0x8b53+t.vecsize-2;
  if(t.basetype==spirv_cross::SPIRType::UInt)return t.vecsize==1?0x1405:0x8dc6+t.vecsize-2;
  return 0x8b56+t.vecsize-1;
 }
 if(t.basetype!=spirv_cross::SPIRType::Float)throw std::runtime_error("Unsupported uniform/attribute scalar type");
 if(t.columns==4&&t.vecsize==4)return 0x8b5c;
 if(t.columns==3&&t.vecsize==3)return 0x8b5b;   // mat3: default uniforms only (see below)
 if(t.columns==2&&t.vecsize==2)return 0x8b5a;   // mat2
 if(t.columns!=1||t.vecsize<1||t.vecsize>4)throw std::runtime_error("Unsupported matrix shape");
 return t.vecsize==1?0x1406:0x8b4e + t.vecsize; // FLOAT, FLOAT_VEC2/3/4
}
// Metal instance_id includes baseInstance; OpenGL gl_InstanceID does not.
// Normalize once in the generated entry point, keeping a named lvalue for
// SPIRV-Cross implicit helper arguments. Rewriting every builtin expression
// corrupts helper declarations and drops their base-instance dependency.
static std::string openGLInstanceID(std::string source){
 const std::string parameter="uint gl_InstanceID [[instance_id]]";
 size_t argument=source.find(parameter);if(argument==std::string::npos)return source;
 if(source.find(parameter,argument+parameter.size())!=std::string::npos)throw std::runtime_error("Ambiguous Metal instance entry point");
 std::string raw="spvLRMetalInstanceID",base="spvLRMetalBaseInstance";while(source.find(raw)!=std::string::npos)raw+="_";while(source.find(base)!=std::string::npos)base+="_";
 source.replace(argument,parameter.size(),"uint "+raw+" [[instance_id]]");
 size_t end=source.find(")\n{",argument);if(end==std::string::npos)throw std::runtime_error("Unrecognized Metal entry-point layout");
 bool nativeBase=source.rfind("uint gl_BaseInstance [[base_instance]]",end)!=std::string::npos;
 if(!nativeBase){const std::string added=", uint "+base+" [[base_instance]]";source.insert(end,added);end+=added.size();}
 source.insert(end+3,std::string("\n    uint gl_InstanceID = ")+raw+" - "+(nativeBase?std::string("gl_BaseInstance"):base)+";\n");
 return source;
}
// Metal fragment position is upper-left. Preserve OpenGL's declared origin
// and pixel-center convention without altering the guest GLSL source.
static std::string openGLFragmentCoord(std::string source,bool upper,bool integerCenter){
 // Keep guest depth precision even though Metal's portable depth attachment is
 // float32. The framebuffer descriptor supplies the fixed-point scale (or 0).
 std::regex entry(R"(fragment ([A-Za-z_][A-Za-z_0-9]*) main0\()");std::smatch match;
 if(!std::regex_search(source,match,entry))throw std::runtime_error("Unrecognized fragment output layout");
 std::string output=match[1],position="gl_FragCoord",frame="spvLRFramebuffer";
 while(source.find(frame)!=std::string::npos)frame+="_";
 size_t argument=source.find("fragment "+output+" main0("),end=source.find(")\n{",argument);
 if(end==std::string::npos)throw std::runtime_error("Unrecognized fragment entry-point layout");
 bool coordinate=source.find("float4 gl_FragCoord [[position]]",argument)<end;
 std::string added=", constant float4& "+frame+" [[buffer(30)]]";
 if(!coordinate){position="spvLRFragmentPosition";while(source.find(position)!=std::string::npos)position+="_";added+=", float4 "+position+" [[position]]";}
 if(source[end-1]=='(')added.erase(0,2);source.insert(end,added);end+=added.size();
 if(coordinate){std::string fix;if(!upper)fix="\n    gl_FragCoord.y = "+frame+".y - gl_FragCoord.y;";if(integerCenter)fix+="\n    gl_FragCoord.xy -= float2(0.5);";source.insert(end+3,fix+"\n");}
 size_t structure=source.find("struct "+output+"\n{"),close=source.find("};",structure);if(structure==std::string::npos||close==std::string::npos)throw std::runtime_error("Missing fragment output structure");
 std::string fields=source.substr(structure,close-structure),depth="spvLRDepth";std::regex depthMember(R"(float ([A-Za-z_][A-Za-z_0-9]*) \[\[depth\([^)]*\)\]\])");std::smatch found;
 bool existing=std::regex_search(fields,found,depthMember);if(existing)depth=found[1];else{while(source.find(depth)!=std::string::npos)depth+="_";source.insert(close,"    float "+depth+" [[depth(any)]];\n");}
 size_t body=source.find(")\n{",source.find("fragment "+output+" main0("));size_t ret=source.find("return out;",body);if(ret==std::string::npos)throw std::runtime_error("Unrecognized fragment return layout");
 std::string value=existing?"out."+depth:position+".z";
 std::string assignment="out."+depth+" = "+frame+".z > 0.0 ? floor(clamp("+value+", 0.0, 1.0) * "+frame+".z + 0.5) / "+frame+".z : "+value+";\n    ";while(ret!=std::string::npos){source.insert(ret,assignment);ret=source.find("return out;",ret+assignment.size()+11);}return source;
}

extern "C" void *lr_glsl_compile(unsigned stage,const char *source,char *log){
 try {
  static std::once_flag initialized;std::call_once(initialized,[]{if(!glslang::InitializeProcess())throw std::runtime_error("GLSL compiler initialization failed");});
  if(stage!=0x8b31&&stage!=0x8b30)throw std::runtime_error("Only vertex and fragment stages supported");
  auto s=std::make_unique<Shader>();s->source=source;s->stage=stage==0x8b31?EShLangVertex:EShLangFragment;s->parsed=std::make_unique<glslang::TShader>(s->stage);
  const char *text=s->source.c_str();s->parsed->setStrings(&text,1);s->parsed->setAutoMapBindings(true);s->parsed->setAutoMapLocations(true);
  if(!s->parsed->parse(GetDefaultResources(),100,false,EShMsgDefault)){error(log,s->parsed->getInfoLog());return nullptr;}
  BlockQualifiers q;s->parsed->getIntermediate()->getTreeRoot()->traverse(&q);s->blockBindings=q.bindings;s->storageBindings=q.storageBindings;s->blockInstances=q.instances;s->blockBooleans=q.booleanTypes;log[0]=0;return s.release();
 }catch(const std::exception &e){error(log,e.what());return nullptr;}
}
extern "C" void lr_glsl_shader_destroy(void *p){delete static_cast<Shader *>(p);}
extern "C" void *lr_glsl_link(void *v,void *f,lr_gl_link *r){
 try {
  auto *vs=static_cast<Shader *>(v),*fs=static_cast<Shader *>(f);
  if(!vs||!fs||vs->stage!=EShLangVertex||fs->stage!=EShLangFragment)throw std::runtime_error("Missing compiled vertex/fragment shader");
  glslang::TProgram linked;linked.addShader(vs->parsed.get());linked.addShader(fs->parsed.get());
  if(!linked.link(EShMsgDefault)||!linked.mapIO()){error(r->log,linked.getInfoLog());return nullptr;}
  auto out=std::make_unique<Program>();unsigned bound=r->attributes;r->attributes=0;r->uniforms=0;
  lr_gl_variable bindings[LR_GL_MAX_ATTRIBUTES];std::memcpy(bindings,r->attribute,sizeof bindings);
  for(int stage=0;stage<2;stage++){
   auto *ir=linked.getIntermediate(stage==0?EShLangVertex:EShLangFragment);
   // ES100 has already been checked by the reference parser/linker. Set only
   // the output IR version after parsing; do not impose Vulkan GLSL semantics.
   glslang::SpvVersion target;target.spv=0x00010000;ir->setSpv(target);
   std::vector<uint32_t> words;glslang::SpvOptions options;options.disableOptimizer=true;glslang::GlslangToSpv(*ir,words,&options);
   spirv_cross::CompilerMSL m(words);auto enabled=m.get_active_interface_variables();m.set_enabled_interface_variables(enabled);auto resources=m.get_shader_resources(enabled);
   m.update_active_builtins();
   if(!resources.storage_images.empty()||!resources.push_constant_buffers.empty()||!resources.separate_images.empty()||!resources.separate_samplers.empty())throw std::runtime_error("Separate images/samplers and storage resources not supported by this backend");
   for(auto &u:resources.storage_buffers){
    if(!m.get_buffer_block_flags(u.id).get(spv::DecorationNonWritable))throw std::runtime_error("Writable shader storage blocks are not supported: "+u.name);
    if(!m.get_type(u.type_id).array.empty())throw std::runtime_error("Storage block arrays are not supported");
    const auto &t=m.get_type(u.base_type_id);std::string name=m.get_name(u.base_type_id);if(name.empty())name=u.name;
    unsigned index=0;while(index<out->storages.count&&name!=out->storages.block[index].name)index++;
    if(index>=LR_GL_MAX_STORAGE_BLOCKS||name.size()>=64)throw std::runtime_error("Storage block reflection limit exceeded");
    lr_gl_storage block{};std::strcpy(block.name,name.c_str());block.min_bytes=m.get_declared_struct_size(t);block.vertex_binding=block.fragment_binding=UINT32_MAX;
    auto &bindings=stage==0?vs->storageBindings:fs->storageBindings;auto binding=bindings.find(name);if(binding==bindings.end())throw std::runtime_error("Storage binding could not be verified");block.binding=binding->second;if(block.binding>=LR_GL_MAX_BLOCK_BINDINGS)throw std::runtime_error("Storage binding exceeds limit");
    for(unsigned j=0;j<t.member_types.size();j++){const auto &mt=m.get_type(t.member_types[j]);if(!mt.array.empty()&&mt.array.back()==0){if(j+1!=t.member_types.size())throw std::runtime_error("Invalid runtime array position");block.runtime_offset=m.type_struct_member_offset(t,j);block.runtime_stride=m.type_struct_member_array_stride(t,j);if(!block.runtime_stride)throw std::runtime_error("Invalid runtime array stride");block.min_bytes=block.runtime_offset+block.runtime_stride;}}
    if(block.min_bytes>LR_GL_MAX_STORAGE_BYTES)throw std::runtime_error("Storage block size exceeds limit");
    if(index==out->storages.count){out->storages.block[index]=block;out->storages.count++;}
    else{auto prior=out->storages.block[index];prior.vertex_binding=prior.fragment_binding=UINT32_MAX;if(std::memcmp(&prior,&block,sizeof block))throw std::runtime_error("Storage block stage layouts/bindings differ");}
    m.set_decoration(u.id,spv::DecorationDescriptorSet,2);m.set_decoration(u.id,spv::DecorationBinding,index);
    spirv_cross::MSLResourceBinding b{};b.stage=stage==0?spv::ExecutionModelVertex:spv::ExecutionModelFragment;b.desc_set=2;b.binding=index;b.msl_buffer=21+index;m.add_msl_resource_binding(b);
    if(stage==0)out->storages.block[index].vertex_binding=21+index;else out->storages.block[index].fragment_binding=21+index;
   }
   for(auto &u:resources.uniform_buffers){
    if(!m.get_type(u.type_id).array.empty())throw std::runtime_error("Uniform block arrays not supported");
    const auto &t=m.get_type(u.base_type_id);std::string name=m.get_name(u.base_type_id);if(name.empty())name=u.name;
    unsigned index=0;while(index<out->blocks.count&&name!=out->blocks.block[index].name)index++;
    if(index>=LR_GL_MAX_BLOCKS||name.size()>=64||t.member_types.size()>LR_GL_MAX_BLOCK_MEMBERS)throw std::runtime_error("Uniform block reflection limit exceeded");
    auto reflected=std::make_unique<lr_gl_block>();auto &block=*reflected;std::strcpy(block.name,name.c_str());block.bytes=(m.get_declared_struct_size(t)+15)&~15u;
    if(!block.bytes||block.bytes>LR_GL_MAX_BLOCK_BYTES)throw std::runtime_error("Uniform block storage exceeds limit");
    auto &qualifiers=stage==0?vs->blockBindings:fs->blockBindings;auto q=qualifiers.find(name);
    if(q==qualifiers.end())throw std::runtime_error("Uniform block layout could not be verified");
    block.binding=q->second;if(block.binding>=LR_GL_MAX_BLOCK_BINDINGS)throw std::runtime_error("Uniform block binding exceeds limit");
    block.vertex_binding=block.fragment_binding=UINT32_MAX;
    bool qualified=vs->blockInstances[name]||fs->blockInstances[name];
    std::function<void(const spirv_cross::SPIRType &,unsigned,const std::string &,unsigned,unsigned)> memberReflect;
    memberReflect=[&](const spirv_cross::SPIRType &parent,unsigned i,const std::string &prefix,unsigned base,unsigned depth){
     if(depth>4)throw std::runtime_error("Uniform block nesting exceeds limit");
     const auto &mt=m.get_type(parent.member_types[i]);std::string mn=prefix+m.get_member_name(parent.self,i);
     if(mt.array.size()>1||(!mt.array.empty()&&(!mt.array_size_literal[0]||!mt.array[0]||mt.array[0]>512)))throw std::runtime_error("Unsupported uniform block array dimensions");
     unsigned offset=base+m.type_struct_member_offset(parent,i);
     if(offset>=block.bytes||m.get_declared_struct_member_size(parent,i)>block.bytes-offset)throw std::runtime_error("Invalid reflected uniform block layout");
     unsigned size=mt.array.empty()?1:mt.array[0],stride=mt.array.empty()?0:m.type_struct_member_array_stride(parent,i);
     if(mt.basetype==spirv_cross::SPIRType::Struct){
      for(unsigned k=0;k<size;k++){std::string element=mn+(mt.array.empty()?"":"["+std::to_string(k)+"]")+".";for(unsigned j=0;j<mt.member_types.size();j++)memberReflect(mt,j,element,offset+k*stride,depth+1);}return;
     }
     if(block.members>=LR_GL_MAX_BLOCK_MEMBERS||mt.width!=32)throw std::runtime_error("Uniform block member reflection limit/type exceeded");
     auto &member=block.member[block.members++];if(!mt.array.empty())mn+="[0]";if(mn.size()>=64)throw std::runtime_error("Uniform member name exceeds limit");std::strcpy(member.name,mn.c_str());member.size=size;
     if(mt.columns>1){if(mt.basetype!=spirv_cross::SPIRType::Float||mt.columns!=mt.vecsize||mt.columns<2||mt.columns>4)throw std::runtime_error("Only square float block matrices supported");member.type=0x8b5a+mt.columns-2;}
     else if(mt.vecsize<1||mt.vecsize>4)throw std::runtime_error("Unsupported block vector");
     else if(mt.basetype==spirv_cross::SPIRType::Float)member.type=mt.vecsize==1?0x1406:0x8b50+mt.vecsize-2;
     else if(mt.basetype==spirv_cross::SPIRType::Int)member.type=mt.vecsize==1?0x1404:0x8b53+mt.vecsize-2;
     else if(mt.basetype==spirv_cross::SPIRType::UInt)member.type=mt.vecsize==1?0x1405:0x8dc6+mt.vecsize-2;
     else throw std::runtime_error("Unsupported block member scalar type");
     const auto &booleans=stage==0?vs->blockBooleans:fs->blockBooleans;auto boolean=booleans.find(blockTypePath(qualified?mn:name+"."+mn));
     if(boolean!=booleans.end()){if(mt.basetype!=spirv_cross::SPIRType::UInt||mt.columns!=1)throw std::runtime_error("Invalid lowered Boolean block storage");member.type=boolean->second;}
     member.offset=offset;member.array_stride=stride;
     if(mt.columns>1){member.matrix_stride=m.type_struct_member_matrix_stride(parent,i);member.row_major=m.has_member_decoration(parent.self,i,spv::DecorationRowMajor);}
    };
    for(unsigned i=0;i<t.member_types.size();i++)memberReflect(t,i,qualified?name+".":"",0,0);
    if(index==out->blocks.count){out->blocks.block[index]=block;out->blocks.count++;}
    else {auto old=std::make_unique<lr_gl_block>(out->blocks.block[index]);old->vertex_binding=old->fragment_binding=UINT32_MAX;if(std::memcmp(old.get(),&block,sizeof block))throw std::runtime_error("Uniform block stage layouts/bindings differ");}
    // Default uniforms occupy slots 1..16, vertex data slot 0. Block slots
    // 17..20 are stable across stages regardless of source descriptor binding.
    m.set_decoration(u.id,spv::DecorationDescriptorSet,1);m.set_decoration(u.id,spv::DecorationBinding,index);
    spirv_cross::MSLResourceBinding b{};b.stage=stage==0?spv::ExecutionModelVertex:spv::ExecutionModelFragment;b.desc_set=1;b.binding=index;b.msl_buffer=17+index;m.add_msl_resource_binding(b);
    if(stage==0)out->blocks.block[index].vertex_binding=17+index;else out->blocks.block[index].fragment_binding=17+index;
   }
   if(stage==0){unsigned used=0,reserved=0;
    for(auto &a:resources.stage_inputs)for(unsigned i=0;i<bound;i++)if(a.name==bindings[i].name){unsigned columns=m.get_type(a.type_id).columns;unsigned bit=((1u<<columns)-1)<<bindings[i].location;if(reserved&bit)throw std::runtime_error("Aliased bound attribute locations");reserved|=bit;}

    for(auto &a:resources.stage_inputs){if(r->attributes>=LR_GL_MAX_ATTRIBUTES||a.name.size()>=64)throw std::runtime_error("Attribute limit exceeded");unsigned location=m.get_decoration(a.id,spv::DecorationLocation);
     bool explicitly_bound=false;
     for(unsigned i=0;i<bound;i++)if(a.name==bindings[i].name){location=bindings[i].location;explicitly_bound=true;}
     if(!explicitly_bound&&(location>=LR_GL_MAX_ATTRIBUTES||((used|reserved)&(1u<<location)))){location=0;while(location<LR_GL_MAX_ATTRIBUTES&&((used|reserved)&(1u<<location)))location++;}
     auto type=gltype(m.get_type(a.type_id));unsigned columns=type==0x8b5c?4:1,mask=((1u<<columns)-1)<<location;if(location+columns>LR_GL_MAX_ATTRIBUTES||(type>=0x8b56&&type<=0x8b59)||type==0x8b5a||type==0x8b5b||location>=LR_GL_MAX_ATTRIBUTES||(used&mask))throw std::runtime_error("Unsupported/aliased attribute locations");used|=mask;m.set_decoration(a.id,spv::DecorationLocation,location);
     auto &entry=r->attribute[r->attributes++];std::memset(&entry,0,sizeof entry);std::strcpy(entry.name,a.name.c_str());entry.type=type;entry.location=location;
    }
   }
   for(auto &u:resources.gl_plain_uniforms){unsigned index=0;while(index<r->uniforms&&u.name!=r->uniform[index].name)index++;
    auto valueType=m.get_type(u.type_id);unsigned elements=1;
    if(!valueType.array.empty()){
     if(valueType.array.size()!=1||!valueType.array_size_literal[0]||!valueType.array[0]||valueType.array[0]>LR_GL_MAX_UNIFORM_ELEMENTS)throw std::runtime_error("Unsupported default uniform array dimensions");
     elements=valueType.array[0];valueType.array.clear();valueType.array_size_literal.clear();
    }
    unsigned type=gltype(valueType),components=lr_gl_type_components(type),scalar=lr_gl_type_kind(type)==3?1:4;
    if((type==0x8b5a||type==0x8b5b)&&elements>1)throw std::runtime_error("Unsupported matrix array");   // MSL column padding differs from GL packing
    unsigned stride=(components==3?4:components)*scalar;
    if(elements*stride>LR_GL_MAX_UNIFORM_ARRAY_BYTES)throw std::runtime_error("Default uniform array storage exceeds limit");
    if(u.name.size()>=64||index>=LR_GL_MAX_UNIFORMS)throw std::runtime_error("Uniform limit exceeded");
    if(index==r->uniforms){auto &entry=r->uniform[r->uniforms++];std::memset(&entry,0,sizeof entry);std::strcpy(entry.name,u.name.c_str());entry.type=type;entry.location=index;entry.vertex_binding=entry.fragment_binding=UINT32_MAX;}
    if(r->uniform[index].type!=type||(out->extents.uniform[index].elements&&out->extents.uniform[index].elements!=elements))throw std::runtime_error("Uniform stage types/dimensions differ");
    out->extents.uniform[index]={elements,stride};
    m.set_decoration(u.id,spv::DecorationDescriptorSet,0);m.set_decoration(u.id,spv::DecorationBinding,index);
    spirv_cross::MSLResourceBinding binding{};binding.stage=stage==0?spv::ExecutionModelVertex:spv::ExecutionModelFragment;binding.desc_set=0;binding.binding=index;binding.msl_buffer=index+1;m.add_msl_resource_binding(binding);
    if(stage==0)r->uniform[index].vertex_binding=index+1;else r->uniform[index].fragment_binding=index+1;
   }
   for(auto &u:resources.sampled_images){
    const auto &type=m.get_type(u.type_id);
    if(type.image.dim!=spv::Dim2D||type.image.arrayed||type.image.ms||type.image.depth||!type.array.empty())throw std::runtime_error("Only sampler2D supported; no arrays, multisampling or shadow samplers");
    unsigned index=0;while(index<r->uniforms&&u.name!=r->uniform[index].name)index++;
    if(index>=LR_GL_MAX_UNIFORMS||u.name.size()>=64)throw std::runtime_error("Uniform limit exceeded");
    if(index==r->uniforms){auto &e=r->uniform[r->uniforms++];std::memset(&e,0,sizeof e);std::strcpy(e.name,u.name.c_str());e.type=0x8b5e;e.location=index;e.vertex_binding=e.fragment_binding=UINT32_MAX;}
    if(r->uniform[index].type!=0x8b5e)throw std::runtime_error("Uniform stage types differ");
    out->extents.uniform[index]={1,4};
    unsigned samplers=0;for(unsigned i=0;i<r->uniforms;i++)samplers+=r->uniform[i].type==0x8b5e;
    if(samplers>4)throw std::runtime_error("Combined sampler limit exceeded");
    m.set_decoration(u.id,spv::DecorationDescriptorSet,0);m.set_decoration(u.id,spv::DecorationBinding,index);
    spirv_cross::MSLResourceBinding b{};b.stage=stage==0?spv::ExecutionModelVertex:spv::ExecutionModelFragment;b.desc_set=0;b.binding=index;b.msl_texture=index;b.msl_sampler=index;m.add_msl_resource_binding(b);
    if(stage==0)r->uniform[index].vertex_binding=index;else r->uniform[index].fragment_binding=index;
   }
   auto opts=m.get_msl_options();opts.platform=spirv_cross::CompilerMSL::Options::iOS;opts.set_msl_version(2,1);opts.ios_support_base_vertex_instance=true;opts.enable_base_index_zero=false;opts.buffer_size_buffer_index=29;
   // The bridge stores native 1/2/3-channel GL targets in canonical RGBA
   // textures. MSL requires outputs at least as wide as the target; upstream
   // padding preserves used components without changing the original GLSL.
   opts.pad_fragment_output_components=stage==1;m.set_msl_options(opts);auto common=m.get_common_options();common.vertex.fixup_clipspace=true;m.set_common_options(common);
   out->msl[stage]=openGLInstanceID(m.compile());if(stage==1)out->msl[stage]=openGLFragmentCoord(out->msl[stage],ir->getOriginUpperLeft(),ir->getPixelCenterInteger());
  }
  out->storages.program=r->name;out->extents.count=r->uniforms;out->extents.program=r->name;
  r->log[0]=0;r->status=1;return out.release();
 }catch(const std::exception &e){r->status=0;error(r->log,e.what());return nullptr;}
}
extern "C" void lr_glsl_blocks(void *p,lr_gl_blocks *r){*r=static_cast<Program *>(p)->blocks;}
extern "C" const char *lr_glsl_msl(void *p,unsigned stage){return static_cast<Program *>(p)->msl[stage].c_str();}
extern "C" void lr_glsl_program_destroy(void *p){delete static_cast<Program *>(p);}

extern "C" void lr_glsl_uniform_extents(void *p,lr_gl_uniform_extents *r){*r=static_cast<Program *>(p)->extents;}

extern "C" void lr_glsl_storages(void *p,lr_gl_storages *r){*r=static_cast<Program *>(p)->storages;}
