#!/bin/sh
set -eu
root=$(CDPATH= cd -- "$(dirname "$0")/../.." && pwd)
work=$(mktemp -d "${TMPDIR:-/tmp}/lr-native-xorg.XXXXXX")
trap 'rm -rf "$work"' EXIT HUP INT TERM
. "$root/app/LinuxRunner/GUISupport.bundle/xorg-native.sh"
# Isolate prerequisites; this is policy testing, not a GPU qualification.
lr_xorg_binary() { echo /test/Xorg; }
glxinfo() { :; }
timeout() { :; }
printf auto > "$work/xorg-gpu"
printf mesa-metal-resident > "$work/gpu-mode"
lr_native_xorg_requested "$work" desktop linuxrunner
lr_native_xorg_requested "$work" desktop i3
lr_native_xorg_requested "$work" desktop gnome
for desktop in plasma hyprland wayfire; do
    if lr_native_xorg_requested "$work" desktop "$desktop"; then exit 1; fi
done
if lr_native_xorg_requested "$work" custom linuxrunner; then exit 1; fi
for mode in mesa mesa-metal-verify invalid; do
    printf %s "$mode" > "$work/gpu-mode"
    if lr_native_xorg_requested "$work" desktop linuxrunner; then exit 1; fi
done
printf mesa-metal-resident > "$work/gpu-mode"
lr_xorg_binary() { return 1; }
if lr_native_xorg_requested "$work" desktop linuxrunner; then exit 1; fi
lr_xorg_binary() { echo /test/Xorg; }
printf off > "$work/xorg-gpu"
if lr_native_xorg_requested "$work" desktop linuxrunner; then exit 1; fi
. "$root/app/LinuxRunner/GUISupport.bundle/gpu-env.sh"
LR_GUI_NATIVE_VIRGL=1
unset LD_PRELOAD LIBGL_ALWAYS_SOFTWARE GALLIUM_DRIVER
lr_gpu_route mesa-metal-resident "$work"
[ "$LR_GUI_GPU_ACTIVE" = distribution-mesa ]
[ -z "${LD_PRELOAD:-}${LIBGL_ALWAYS_SOFTWARE:-}${GALLIUM_DRIVER:-}" ]
echo 'NATIVE_XORG_POLICY PASS requested mode, supported sessions, missing prerequisite and native route'
