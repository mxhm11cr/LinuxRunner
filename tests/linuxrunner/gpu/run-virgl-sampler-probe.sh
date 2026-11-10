#!/bin/bash
# Native macOS prerequisite check; does not install or launch the iPad app.
# Build the pinned dependencies first: scripts/build-gpu-deps.sh macosx.
set -euo pipefail
cd "$(dirname "$0")/../../.."
[[ $(uname -s) == Darwin && $(uname -m) == arm64 ]] || {
    echo 'This probe requires an Apple Silicon Mac.' >&2; exit 2;
}
out=${LR_VIRGL_PROBE_OUTPUT:-build/virgl-sampler-probe}
mkdir -p "$out"
for archive in libangle.a libepoxy.a libvirglrenderer.a; do
    [[ -f build/gpu/macosx/lib/$archive ]] || {
        echo 'Build the pinned macosx GPU dependencies first.' >&2; exit 2;
    }
done
clang -O2 -g -DEGL_EGLEXT_PROTOTYPES -I. \
    -Ibuild/gpu/macosx/include/angle -Ibuild/gpu/macosx/include \
    tests/linuxrunner/gpu/virgl_sampler_probe.c graphics/virgl_host.c \
    build/gpu/macosx/lib/libvirglrenderer.a build/gpu/macosx/lib/libepoxy.a \
    -Wl,-force_load,build/gpu/macosx/lib/libangle.a -Wl,-export_dynamic -lc++ \
    -framework Foundation -framework Metal -framework QuartzCore \
    -framework IOSurface -framework CoreGraphics -framework IOKit -framework CoreVideo \
    -o "$out/virgl-sampler-probe"
exec "$out/virgl-sampler-probe"
