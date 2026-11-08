# LinuxRunner: connect terminal login shells to the running LinuxRunner desktop.
# session.sh installs this file as /etc/profile.d/linuxrunner-desktop.sh. The
# desktop session (running as the desktop user) records its display in
# ~/.cache/linuxrunner/desktop.env. Values are parsed, never evaluated, each is
# validated, and they apply only while that X server's socket exists.
# Root shells get the desktop's DISPLAY and XAUTHORITY (to open windows on it)
# but never the desktop user's runtime directory or session bus.
# In a shell opened before the desktop, run: linuxrunner_desktop
_lr_desktop_read() {
    _lr_d= _lr_x= _lr_r= _lr_b= _lr_w=
    [ -f "$1" ] && [ ! -L "$1" ] && [ -r "$1" ] || return 1
    while IFS= read -r _lr_line; do
        _lr_v=${_lr_line#*=}
        case "$_lr_v" in *[!A-Za-z0-9/._=,:-]*) return 1;; esac
        case "$_lr_line" in
            DISPLAY=:*) _lr_n=${_lr_v#:}; case "$_lr_n" in ''|*[!0-9]*) return 1;; esac; _lr_d=$_lr_v;;
            XAUTHORITY=/home/mobile/Downloads/.linuxrunner/gui/session-*/authority) _lr_x=$_lr_v;;
            XDG_RUNTIME_DIR=/tmp/lr-runtime.*) _lr_r=$_lr_v;;
            DBUS_SESSION_BUS_ADDRESS=unix:*) _lr_b=$_lr_v;;
            WAYLAND_DISPLAY=/tmp/lrh-*/wayland-*|WAYLAND_DISPLAY=/tmp/lrw-*/wayland-*|WAYLAND_DISPLAY=/tmp/lr-runtime.*/wayland-*) _lr_w=$_lr_v;;
            *) return 1;;
        esac
    done < "$1"
    case "$_lr_x$_lr_r$_lr_w" in *..*) return 1;; esac
    if [ -n "$_lr_d" ]; then
        [ -n "$_lr_x" ] && [ -r "$_lr_x" ] && [ -S "/tmp/.X11-unix/X${_lr_d#:}" ]
    else
        # KWin's managed virtual-output session has no X server. Only its
        # selected user's owned, live Wayland socket may provide this context.
        [ "$(id -u)" != 0 ] && [ -O "$1" ] && [ -n "$_lr_r" ] && [ -O "$_lr_r" ] && [ -n "$_lr_b" ] || return 1
        case "$_lr_w" in "$_lr_r"/wayland-*) [ -S "$_lr_w" ];; *) return 1;; esac
    fi
}
linuxrunner_desktop() {
    _lr_found=
    if [ "$(id -u)" = 0 ]; then
        for _lr_f in /root/.cache/linuxrunner/desktop.env /home/*/.cache/linuxrunner/desktop.env; do
            if _lr_desktop_read "$_lr_f"; then _lr_found=$_lr_f; break; fi
        done
    elif [ -n "${HOME:-}" ] && _lr_desktop_read "$HOME/.cache/linuxrunner/desktop.env"; then
        _lr_found=$HOME/.cache/linuxrunner/desktop.env
    fi
    if [ -z "$_lr_found" ]; then
        [ "${1:-}" = --quiet ] || echo 'LinuxRunner desktop is not running for this account; open it with Open Desktop in the LinuxRunner app.'
        unset _lr_d _lr_x _lr_r _lr_b _lr_v _lr_n _lr_line _lr_f _lr_found
        return 1
    fi
    if [ -n "$_lr_d" ]; then
        export DISPLAY="$_lr_d" XAUTHORITY="$_lr_x" XDG_SESSION_TYPE=x11
        export QT_QPA_PLATFORM=${QT_QPA_PLATFORM:-xcb}
    else unset DISPLAY XAUTHORITY; fi
    export QT_FORCE_STDERR_LOGGING=${QT_FORCE_STDERR_LOGGING:-1}
    if [ "$(id -u)" != 0 ]; then
        if [ -n "$_lr_r" ] && [ -d "$_lr_r" ] && [ -O "$_lr_r" ]; then export XDG_RUNTIME_DIR="$_lr_r"; fi
        [ -z "$_lr_b" ] || export DBUS_SESSION_BUS_ADDRESS="$_lr_b"
        _lr_wayland "$_lr_w"
    fi
    [ "${1:-}" = --quiet ] || echo "LinuxRunner desktop: DISPLAY=${DISPLAY:-none} WAYLAND_DISPLAY=${WAYLAND_DISPLAY:-none}; graphical programs started here open on the desktop."
    unset _lr_d _lr_x _lr_r _lr_b _lr_w _lr_v _lr_n _lr_line _lr_f _lr_found
    return 0
}
# Wayland desktops (Hyprland, Wayfire on the GPU) add their socket: programs
# then open inside the compositor (natively for GTK, Qt, Firefox, Electron);
# X11-only programs keep DISPLAY. Only the desktop user's own live socket.
_lr_wayland() {
    [ -n "$1" ] && [ -S "$1" ] && [ -O "${1%/*}" ] || return 1
    export WAYLAND_DISPLAY="$1" XDG_SESSION_TYPE=wayland MOZ_ENABLE_WAYLAND=1 ELECTRON_OZONE_PLATFORM_HINT=auto
    export QT_QPA_PLATFORM="wayland;xcb" GDK_BACKEND="wayland,x11"
    # The desktop composites on the GPU (virtio-gpu card); programs render with
    # software OpenGL by default, as complex applications' virgl contexts are
    # not reliable yet (Firefox's failed on ANGLE). LINUXRUNNER_GPU_APPS=1 opts in.
    if [ -c /dev/dri/card1 ] && [ "${LINUXRUNNER_GPU_APPS:-0}" != 1 ]; then export LIBGL_ALWAYS_SOFTWARE=1; fi
}
# For launches that already have the session's DISPLAY (launch.sh).
linuxrunner_wayland() {
    [ "$(id -u)" != 0 ] && [ -n "${HOME:-}" ] && _lr_desktop_read "$HOME/.cache/linuxrunner/desktop.env" && _lr_wayland "$_lr_w"
    _lr_status=$?; unset _lr_d _lr_x _lr_r _lr_b _lr_w _lr_v _lr_n _lr_line; return $_lr_status
}
linuxrunner_desktop --quiet || true
