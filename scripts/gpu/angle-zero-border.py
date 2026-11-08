#!/usr/bin/env python3
"""Compile two ANGLE object replacements for a private zero-border bridge.

The full GLES border-color extension stays unadvertised. Only virglrenderer's
validated, all-zero sampler border uses this bridge to Metal's clamp-to-zero.
Upstream sources and their normal build objects remain unchanged.
"""
import pathlib
import shlex
import subprocess


def build(out_dir, members):
    source = out_dir.parent.parent
    work = out_dir / 'linuxrunner-zero-border'
    work.mkdir(exist_ok=True)
    replacements = {}
    global_objects = [p for p in members if p.endswith('/global_state.o')]
    assert len(global_objects) == 1, 'unexpected ANGLE global-state inputs'
    units = [
        ('src/libGLESv2/global_state.cpp', str(pathlib.Path(global_objects[0]).relative_to(out_dir))),
        ('src/libANGLE/renderer/metal/mtl_utils.mm',
         'obj/src/libANGLE/renderer/metal/angle_metal_backend/mtl_utils.o'),
    ]
    for name, target in units:
        original = source / name
        content = original.read_text()
        if original.name == 'global_state.cpp':
            content += r'''

// LinuxRunner private embedder operation, not a GLES extension. The GPU
// thread owns these contexts. Arbitrary border colors remain unsupported.
#include "libANGLE/Sampler.h"
extern "C" __attribute__((visibility("default")))
GLboolean LinuxRunnerANGLESetSamplerZeroBorder(GLuint sampler, GLenum pname)
{
    gl::Context *context = gl::GetValidGlobalContext();
    if (!context || context->getDisplay()->getAttributeMap().get(
            EGL_PLATFORM_ANGLE_TYPE_ANGLE, EGL_PLATFORM_ANGLE_TYPE_DEFAULT_ANGLE) !=
            EGL_PLATFORM_ANGLE_TYPE_METAL_ANGLE || !context->isSampler({sampler}))
        return GL_FALSE;
    gl::SamplerParameter parameter;
    switch (pname) {
        case GL_TEXTURE_WRAP_S: parameter = gl::SamplerParameter::WrapS; break;
        case GL_TEXTURE_WRAP_T: parameter = gl::SamplerParameter::WrapT; break;
        case GL_TEXTURE_WRAP_R: parameter = gl::SamplerParameter::WrapR; break;
        default: return GL_FALSE;
    }
    const gl::Sampler *object = context->getSampler({sampler});
    if (object) {
        const auto &color = object->getBorderColor();
        bool zero = false;
        switch (color.type) {
            case angle::ColorGeneric::Type::Float:
                zero = color.colorF.red == 0 && color.colorF.green == 0 &&
                       color.colorF.blue == 0 && color.colorF.alpha == 0; break;
            case angle::ColorGeneric::Type::Int:
                zero = color.colorI.red == 0 && color.colorI.green == 0 &&
                       color.colorI.blue == 0 && color.colorI.alpha == 0; break;
            case angle::ColorGeneric::Type::UInt:
                zero = color.colorUI.red == 0 && color.colorUI.green == 0 &&
                       color.colorUI.blue == 0 && color.colorUI.alpha == 0; break;
        }
        if (!zero) return GL_FALSE;
    }
    context->samplerParameteri({sampler}, parameter, GL_CLAMP_TO_BORDER_EXT);
    return GL_TRUE;
}
'''
        else:
            old = '        case GL_CLAMP_TO_EDGE:\n            return MTLSamplerAddressModeClampToEdge;'
            assert content.count(old) == 1, 'ANGLE sampler address conversion changed'
            content = content.replace(old, '''        // Reachable only through the validated private zero-border bridge.
        case GL_CLAMP_TO_BORDER_EXT:
            return MTLSamplerAddressModeClampToZero;
''' + old)
        patched = work / original.name
        patched.write_text(content)
        command = subprocess.check_output(
            ['ninja', '-C', str(out_dir), '-t', 'commands', target], text=True).splitlines()[-1]
        args = shlex.split(command)
        output = work / (original.stem + '.o')
        compile_source = str(pathlib.Path('../..') / name)
        assert args.count(compile_source) == 1 and args[args.index('-o') + 1] == target
        args[args.index(compile_source)] = str(patched)
        args[args.index('-o') + 1] = str(output)
        # These replacement files do not update Ninja's upstream dependency files.
        cleaned = []
        skip = False
        for arg in args:
            if skip:
                skip = False
                continue
            if arg == '-MF':
                skip = True
            elif arg != '-MMD':
                cleaned.append(arg)
        subprocess.run(cleaned, cwd=out_dir, check=True)
        replacements[str(out_dir / target)] = str(output)
    return replacements
