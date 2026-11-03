#!/bin/bash
# Manual experiment in a disposable SteamFrame root, as a dedicated regular user.
# Writes test-user configuration and reports; never run in an existing user environment.
set -eu
. /etc/os-release
[ "$ID" = steamos ]
[ "$(id -u)" -ne 0 ] || { echo 'Use a dedicated regular guest user'; exit 2; }
export DISPLAY=:17 XDG_SESSION_TYPE=x11
export QT_QPA_PLATFORM=xcb QT_QUICK_BACKEND=software QT_OPENGL=software
export QT_XCB_GL_INTEGRATION=none QT_X11_NO_MITSHM=1 KWIN_COMPOSE=N LIBGL_ALWAYS_SOFTWARE=1
export XDG_RUNTIME_DIR
XDG_RUNTIME_DIR=$(mktemp -d /tmp/lr-steamos-runtime.XXXXXXXX)
report="$HOME/steamframe-probe"
mkdir -p "$report/framebuffer" "$HOME/.config/autostart"
: > "$report/clients.log"
printf '[General]\nsystemdBoot=false\n' > "$HOME/.config/startkderc"
# This milestone exercises the desktop and terminal; keep the Steam client out of autostart.
printf '[Desktop Entry]\nType=Application\nHidden=true\n' > "$HOME/.config/autostart/steam.desktop"
Xvfb "$DISPLAY" -screen 0 960x540x24 -nolisten tcp -ac -noreset -fbdir "$report/framebuffer" -extension MIT-SHM > "$report/xvfb.log" 2>&1 & xpid=$!
pid= terminal=
cleanup() {
    [ -z "$terminal" ] || kill "$terminal" 2>/dev/null || true
    [ -z "$pid" ] || kill "$pid" 2>/dev/null || true
    kill "$xpid" 2>/dev/null || true
    wait "$xpid" 2>/dev/null || true
}
trap cleanup EXIT
for ((i=0;i<100;i++)); do
    [ ! -S /tmp/.X11-unix/X17 ] || break
    kill -0 "$xpid" || { cat "$report/xvfb.log"; exit 3; }
    sleep .1
done
[ -S /tmp/.X11-unix/X17 ]
dbus-run-session -- startplasma-x11 > "$report/plasma.log" 2>&1 & pid=$!
sleep 30
kill -0 "$pid"
konsole --nofork --separate --hide-menubar --hide-tabbar -e /bin/bash --noprofile --norc -ic 'printf "SteamOS ARM64 in LinuxRunner\n"; cat /etc/os-release; uname -m; exec /bin/bash --noprofile --norc' > "$report/konsole.log" 2>&1 & terminal=$!
sleep 20
xprop -root _NET_CLIENT_LIST _NET_SUPPORTING_WM_CHECK > "$report/windows.log"
for window in $(sed -n 's/.*window id # //p' "$report/windows.log" | tr -d ','); do
    xprop -id "$window" WM_CLASS _NET_WM_NAME >> "$report/clients.log" 2>&1 || true
done
cp "$report/framebuffer/Xvfb_screen0" "$report/desktop.xwd"
grep -qi plasmashell "$report/clients.log"
grep -qi konsole "$report/clients.log"
printf 'STEAMOS_DESKTOP=PASS (X11 software rendering; host runtime experiment)\n'
