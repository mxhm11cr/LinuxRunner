#!/bin/sh
# Firefox uses software rendering on the active compositor display.
set -eu
[ "$#" -gt 0 ] || exit 2
case "${1##*/}" in firefox|firefox-esr) ;; *) echo 'Expected a Firefox executable'; exit 2;; esac
socket=${WAYLAND_DISPLAY:-}
case "$socket" in /*|'') ;; *) socket="${XDG_RUNTIME_DIR:-}/$socket";; esac
if [ -n "$socket" ] && [ -S "$socket" ]; then
    export MOZ_ENABLE_WAYLAND=1 GDK_BACKEND=wayland
else
    export MOZ_ENABLE_WAYLAND=0 GDK_BACKEND=x11
    unset WAYLAND_DISPLAY
fi
export LIBGL_ALWAYS_SOFTWARE=1 GALLIUM_DRIVER=softpipe
export LR_MESA_METAL_MODE=off LR_GUI_GPU_ACTIVE=mesa
unset MESA_LOADER_DRIVER_OVERRIDE
exec "$@"
