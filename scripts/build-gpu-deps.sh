#!/bin/bash
# GPU acceleration dependencies (LINUXRUNNER_VIRGL=YES builds): upstream
# virglrenderer, libepoxy and ANGLE (Metal backend only), built from official
# sources at pinned revisions into build/gpu/<platform>/{lib,include}.
#   scripts/build-gpu-deps.sh <iphoneos|iphonesimulator|macosx>
# GPU_DEPS must hold the checkouts: virglrenderer, libepoxy, angle (synced with
# depot_tools: python3 scripts/bootstrap.py && gclient sync) and depot_tools.
# Everything is static: the app loads no libraries at run time. ANGLE becomes
# one archive (scripts/gpu/angle-static-archive.py, the inputs of ANGLE's own
# libGLESv2 link plus libEGL_static) and libepoxy finds EGL/GLES in the process
# image on both iOS and the macOS test runtime (the process-image patch is
# applied to a copy; the checkouts stay pristine).
set -euo pipefail
cd "$(dirname "$0")/.."
platform=${1:?platform}
: "${GPU_DEPS:?Set GPU_DEPS to the directory holding the upstream checkouts}"
VIRGL_REV=e0a1caf EPOXY_REV=$(git -C "$GPU_DEPS/libepoxy" rev-parse --short HEAD) ANGLE_REV=feebf60fe0
for dep in "virglrenderer:$VIRGL_REV" "angle:$ANGLE_REV"; do
    name=${dep%%:*} want=${dep##*:}
    have=$(git -C "$GPU_DEPS/$name" rev-parse --short=${#want} HEAD)
    [[ $have == "$want" ]] || { echo "$name is at $have, expected $want" >&2; exit 2; }
done
out=$PWD/build/gpu/$platform
work=$out/work
mkdir -p "$out/lib" "$out/include" "$work"
sdk=$(xcrun --sdk "$platform" --show-sdk-path)
case $platform in
    iphoneos) target=arm64-apple-ios15.0; angle_env=device; os=ios ;;
    iphonesimulator) target=arm64-apple-ios15.0-simulator; angle_env=simulator; os=ios ;;
    macosx) target=arm64-apple-macos12.0; angle_env=; os=mac ;;
    *) echo "unsupported platform $platform" >&2; exit 2 ;;
esac

# ANGLE (Metal backend only, no debug info) as one static archive.
angle_out=out/lr-$platform
args="is_debug=false symbol_level=0 target_cpu=\"arm64\" angle_enable_metal=true angle_enable_vulkan=false angle_enable_gl=false"
args+=" angle_enable_null=false angle_enable_swiftshader=false angle_build_tests=false is_component_build=false"
args+=" angle_enable_wgpu=false"
args+=" angle_has_frame_capture=false use_siso=false"
[[ $os == ios ]] && args+=" target_os=\"ios\" target_environment=\"$angle_env\" ios_enable_code_signing=false ios_deployment_target=\"15.0\""
(cd "$GPU_DEPS/angle" && PATH="$GPU_DEPS/depot_tools:$PATH" buildtools/mac/gn gen "$angle_out" --args="$args" >/dev/null &&
    ninja -C "$angle_out" libGLESv2 libEGL_static)
python3 scripts/gpu/angle-static-archive.py "$GPU_DEPS/angle/$angle_out" "$out/lib/libangle.a"
rsync -a --delete "$GPU_DEPS/angle/include/" "$out/include/angle/"

# Meson cross file for this SDK.
cat > "$work/cross.txt" <<EOF
[binaries]
c = ['xcrun', '--sdk', '$platform', 'clang']
ar = ['xcrun', '--sdk', '$platform', 'ar']
strip = ['xcrun', '--sdk', '$platform', 'strip']
pkg-config = 'pkg-config'
[built-in options]
c_args = ['-target', '$target', '-isysroot', '$sdk', '-I$out/include/angle', '-DEPOXY_EGL_IN_PROCESS_IMAGE', '-DLINUXRUNNER_ANGLE_ZERO_BORDER']
c_link_args = ['-target', '$target', '-isysroot', '$sdk']
default_library = 'static'
[properties]
needs_exe_wrapper = true
[host_machine]
system = 'darwin'
subsystem = '$([[ $os == ios ]] && echo ios || echo macos)'
cpu_family = 'aarch64'
cpu = 'arm64'
endian = 'little'
EOF

# Static ANGLE needs the same symbol lookup on iOS and in the macOS test host.
rm -rf "$work/libepoxy" && cp -R "$GPU_DEPS/libepoxy" "$work/libepoxy"
(cd "$work/libepoxy" && git apply "$OLDPWD/scripts/gpu/libepoxy-ios-process-image.patch")
meson setup --wipe "$work/epoxy-build" "$work/libepoxy" --cross-file "$work/cross.txt" --prefix="$out" \
    -Degl=yes -Dglx=no -Dx11=false -Dtests=false -Ddocs=false >/dev/null
ninja -C "$work/epoxy-build" install >/dev/null
rm -rf "$work/virglrenderer" && cp -R "$GPU_DEPS/virglrenderer" "$work/virglrenderer"
(cd "$work/virglrenderer" && git apply "$OLDPWD/scripts/gpu/virglrenderer-angle-zero-border.patch")
PKG_CONFIG_LIBDIR=$out/lib/pkgconfig meson setup --wipe "$work/virgl-build" "$work/virglrenderer" --cross-file "$work/cross.txt" \
    --prefix="$out" -Dplatforms=egl -Dvenus=false -Dvideo=false -Dtests=false >/dev/null
ninja -C "$work/virgl-build" install >/dev/null
printf 'virglrenderer %s\nlibepoxy %s\nangle %s\n' "$VIRGL_REV" "$EPOXY_REV" "$ANGLE_REV" > "$out/REVISIONS"
ls -la "$out/lib"
