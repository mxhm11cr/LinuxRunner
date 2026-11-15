# Shared X11 server discovery. Never substitute another distro's Mesa libraries.
lr_xorg_binary() {
    for lr_xorg in /usr/lib/xorg/Xorg /usr/lib/Xorg /usr/libexec/Xorg; do
        if [ -x "$lr_xorg" ]; then printf '%s\n' "$lr_xorg"; return 0; fi
    done
    return 1
}
lr_native_xorg_requested() {
    [ "$2" = desktop ] || return 1
    [ "$(cat "$1/xorg-gpu" 2>/dev/null || true)" = auto ] || return 1
    [ "$(cat "$1/gpu-mode" 2>/dev/null || true)" = mesa-metal-resident ] || return 1
    # X11 sessions on the distribution's Xorg (modesetting + glamor on the
    # virtual GPU). GNOME Shell runs as the X11 compositor there, so Mutter
    # composites with OpenGL on virgl/ANGLE/Metal instead of llvmpipe. Plasma
    # keeps its separately qualified compositor path.
    case "$3" in linuxrunner|i3|xfce|lxqt|gnome|mate|cinnamon|budgie|cosmic) ;; *) return 1;; esac
    lr_xorg_binary >/dev/null || return 1
    command -v timeout >/dev/null 2>&1 || return 1
    command -v glxinfo >/dev/null 2>&1
}
