#!/bin/sh
# Firefox uses software X11 inside the compositor. The desktop keeps KMS.
set -eu
[ "$#" -gt 0 ] || exit 2
case "${1##*/}" in firefox|firefox-esr) ;; *) echo 'Expected a Firefox executable'; exit 2;; esac
export MOZ_ENABLE_WAYLAND=0 GDK_BACKEND=x11 LIBGL_ALWAYS_SOFTWARE=1 GALLIUM_DRIVER=softpipe
export LR_MESA_METAL_MODE=off LR_GUI_GPU_ACTIVE=mesa
unset WAYLAND_DISPLAY MESA_LOADER_DRIVER_OVERRIDE
exec "$@"
