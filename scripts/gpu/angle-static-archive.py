#!/usr/bin/env python3
# One static archive of ANGLE (EGL + GLES on Metal) for apps that cannot load
# frameworks by name: exactly the objects and archives ANGLE's own link of the
# libGLESv2 framework uses (from its generated ninja file), plus libEGL_static
# (egl* entry points calling into it directly, no runtime loader).
# libEGL_static declares its entry points with EGLAPI= (hidden): they link but
# are not exported, and libepoxy finds EGL with dlsym. libEGL_autogen.cpp is
# compiled again with ANGLE's own command for it, EGLAPI set to default
# visibility as in ANGLE's shared libEGL, and that object replaces it.
#   angle-static-archive.py ANGLE_OUT_DIR OUTPUT.a
import pathlib, re, shlex, subprocess, sys
import importlib.util
out_dir = pathlib.Path(sys.argv[1]).resolve(); target = pathlib.Path(sys.argv[2]).resolve()
# iOS emits a framework target; macOS emits libGLESv2.dylib. Both describe
# the same library's link inputs and must work for host GPU qualification.
ninja_files = [out_dir / 'obj' / name for name in
               ('libGLESv2_framework_shared_library.ninja', 'libGLESv2.ninja')]
ninja_file = next((p for p in ninja_files if p.is_file()), None)
if ninja_file is None:
    sys.exit('missing ANGLE libGLESv2 link rule')
lines = [line for line in ninja_file.read_text().splitlines()
         if line.startswith('build ') and ': solink ' in line]
if len(lines) != 1:
    sys.exit('ambiguous ANGLE libGLESv2 link rule')
line = lines[0]
inputs = line.split(': solink ', 1)[1].split(' | ')[0].split(' || ')[0].split()
llvm_ar = out_dir.parent.parent / 'third_party/llvm-build/Release+Asserts/bin/llvm-ar'
def expand(path):
    # GN builds thin archives (members are references to object files).
    if path.suffix == '.a' and path.read_bytes()[:8] == b'!<thin>\n':
        listed = subprocess.run([str(llvm_ar), 't', str(path)], check=True, capture_output=True, text=True).stdout.split()
        return [str((path.parent / m).resolve()) if not pathlib.Path(m).is_absolute() else m for m in listed]
    return [str(path)]
egl_object = 'obj/libEGL_static/libEGL_autogen.o'
command = subprocess.run(['ninja', '-C', str(out_dir), '-t', 'commands', egl_object], check=True, capture_output=True, text=True).stdout.strip().splitlines()[-1]
args = shlex.split(command)
assert args.count('-DEGLAPI=') == 1 and args[args.index('-o') + 1] == egl_object, 'unexpected libEGL compile command'
exported = out_dir / 'obj/libEGL_static/libEGL_autogen_exported.o'
args[args.index('-DEGLAPI=')] = '-DEGLAPI=__attribute__((visibility("default")))'
args[args.index('-o') + 1] = str(exported)
args = [a for i, a in enumerate(args) if a != '-MMD' and not (i and args[i - 1] == '-MF') and a != '-MF']
subprocess.run(args, check=True, cwd=out_dir)
members = []
for p in inputs + ['obj/libEGL_static.a']:
    # the app links its own compiler runtime
    if p.endswith(('.o', '.a')) and 'libclang_rt' not in p:
        members += [str(exported) if m.endswith('/' + egl_object) else m for m in expand(out_dir / p)]
spec = importlib.util.spec_from_file_location('angle_zero_border', pathlib.Path(__file__).with_name('angle-zero-border.py'))
zero_border = importlib.util.module_from_spec(spec)
spec.loader.exec_module(zero_border)
replacements = zero_border.build(out_dir, members)
assert all(members.count(original) == 1 for original in replacements), 'unexpected ANGLE zero-border inputs'
members = [replacements.get(member, member) for member in members]
missing = [m for m in members if not pathlib.Path(m).exists()]
if missing: sys.exit('missing inputs: ' + ' '.join(missing[:5]))
target.parent.mkdir(parents=True, exist_ok=True)
subprocess.run(['xcrun', 'libtool', '-static', '-no_warning_for_no_symbols', '-o', str(target)] + list(dict.fromkeys(members)), check=True)
hidden = subprocess.run(['nm', '-m', str(exported)], check=True, capture_output=True, text=True).stdout
assert 'external _eglGetProcAddress' in hidden and 'private external _eglGetProcAddress' not in hidden, 'egl entry points not exported'
print(target, len(members), 'inputs')
