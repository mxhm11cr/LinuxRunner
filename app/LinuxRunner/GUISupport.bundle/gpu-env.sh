# Guest graphics routing shared by launch.sh and session.sh. Sourced, not run.
#
# lr_gpu_route MODE SUPPORT_DIR [STATS_DIR]
#   mesa                 software Mesa only (default)
#   mesa-metal-resident  Mesa bridge, eligible GL draws on Metal with render
#                        targets kept on the GPU (experimental)
#   mesa-metal-verify    Mesa bridge, each Metal draw checked against Mesa
#                        (development diagnostics; slow)
# Exports the environment for MODE into the calling shell. A GPU mode that
# cannot be used here falls back to Mesa with a GPU_ROUTING line saying why; it
# never stops the application. STATS_DIR receives per-process GPU statistics
# (<pid>.txt) that separate Metal draws from Mesa draws.
lr_gpu_route() {
    lr_mode=${1:-mesa}; lr_support=$2; lr_stats=${3:-}
    case "$lr_mode" in
     ''|mesa|software) export LR_GUI_GPU_ACTIVE=mesa; echo "GPU_ROUTING: requested=mesa active=mesa"; return 0;;
     mesa-metal-resident|mesa-metal-verify) ;;
     *) export LR_GUI_GPU_ACTIVE=mesa; echo "GPU_ROUTING: requested=$lr_mode active=mesa reason=unknown-mode"; return 0;;
    esac
    lr_provider="$lr_support/liblinuxrunner-mesa-metal.so" lr_why=
    # Distributions whose own Mesa passed the bridge contract
    # (gpu-mesa-metal-check.sh: GLX core context, Metal draws verified against
    # Mesa, pixels/HDR/sRGB): Debian, Fedora, Arch Linux ARM, Kali (Build 902).
    case "$(sed -n 's/^ID=//p' /etc/os-release 2>/dev/null | tr -d '"')" in
        debian|fedora|arch|archarm|kali) ;;
        *) lr_why=distribution-not-yet-qualified;;
    esac
    [ -n "$lr_why" ] || [ -r "$lr_provider" ] || lr_why=provider-missing-refresh-gui-support
    [ -n "$lr_why" ] || [ -c /dev/linuxrunner-gpu ] || lr_why=metal-device-node-missing
    [ -n "$lr_why" ] || { [ -r /dev/linuxrunner-gpu ] && [ -w /dev/linuxrunner-gpu ]; } || lr_why=metal-device-not-accessible-to-$(id -un)
    if [ -n "$lr_why" ]; then
        export LR_GUI_GPU_ACTIVE=mesa
        echo "GPU_ROUTING: requested=$lr_mode active=mesa reason=$lr_why"
        return 0
    fi
    export LIBGL_ALWAYS_SOFTWARE=1 GALLIUM_DRIVER=llvmpipe
    if [ "$lr_mode" = mesa-metal-resident ]; then
        export LR_MESA_METAL_MODE=replace LR_MESA_METAL_RESIDENT=1 LR_MESA_METAL_MAX_DRAWS=unlimited
    else
        export LR_MESA_METAL_MODE=verify LR_MESA_METAL_MAX_DRAWS=128
        unset LR_MESA_METAL_RESIDENT
    fi
    if [ -n "$lr_stats" ] && mkdir -p "$lr_stats" 2>/dev/null && [ -w "$lr_stats" ]; then
        export LR_MESA_METAL_STATS_DIR="$lr_stats"
    fi
    # Scoped to this process tree. The X server and VNC bridge are started
    # before the desktop applies this, so they never load the bridge.
    case ":${LD_PRELOAD:-}:" in *":$lr_provider:"*) ;; *) export LD_PRELOAD="$lr_provider${LD_PRELOAD:+:$LD_PRELOAD}";; esac
    export LR_GUI_GPU_ACTIVE=$lr_mode
    echo "GPU_ROUTING: requested=$lr_mode active=$lr_mode provider=$lr_provider stats=${LR_MESA_METAL_STATS_DIR:-none}"
}
