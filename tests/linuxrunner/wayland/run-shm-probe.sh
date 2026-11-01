#!/bin/sh
# Run inside a disposable guest; no system session or display gets replaced.
# Optional first argument: headless (default), or x11 in an authenticated session.
set -eu
backend=${1:-headless}
case "$backend" in headless|x11) ;; *) echo 'Expected headless or x11' >&2; exit 2;; esac
shell=${LR_WESTON_SHELL:-kiosk-shell.so}
case "$shell" in kiosk-shell.so|desktop-shell.so) ;; *) echo Unsupported-test-shell; exit 2;; esac
command -v weston >/dev/null
command -v wayland-scanner >/dev/null
command -v timeout >/dev/null
command -v setsid >/dev/null
base=$(CDPATH= cd -- "$(dirname "$0")" && pwd)
work=$(mktemp -d "${TMPDIR:-/tmp}/linuxrunner-wayland.XXXXXX")
chmod 700 "$work"
compositor=
cleanup() {
 trap - EXIT HUP INT TERM
 if [ -n "$compositor" ]; then
  kill -TERM -- "-$compositor" 2>/dev/null || :
  (sleep 3; kill -KILL -- "-$compositor" 2>/dev/null || :) & watchdog=$!
  if wait "$compositor" 2>/dev/null; then echo WAYLAND_COMPOSITOR_STOPPED; else echo "WAYLAND_COMPOSITOR_STOP_STATUS=$? (137 means watchdog forced shutdown)"; fi
  kill "$watchdog" 2>/dev/null || :; wait "$watchdog" 2>/dev/null || :
 fi
 if [ -f "$work/weston.log" ]; then cat "$work/weston.log"; fi
 rm -rf "$work"
}
trap cleanup EXIT
trap 'exit 129' HUP
trap 'exit 130' INT
trap 'exit 143' TERM
protocol=/usr/share/wayland-protocols/stable/xdg-shell/xdg-shell.xml
wayland-scanner client-header "$protocol" "$work/xdg-shell-client-protocol.h"
wayland-scanner private-code "$protocol" "$work/xdg-shell-protocol.c"
cc -O2 -Wall -Wextra -I"$work" "$base/shm-client.c" "$work/xdg-shell-protocol.c" $(pkg-config --cflags --libs wayland-client) -o "$work/client"
export XDG_RUNTIME_DIR="$work" WAYLAND_DISPLAY=linuxrunner-probe
setsid weston --backend="$backend" --renderer=pixman --socket="$WAYLAND_DISPLAY" --idle-time=0 --width=640 --height=480 --no-config --shell="$shell" --log="$work/weston.log" &
compositor=$!
i=0
until [ -S "$XDG_RUNTIME_DIR/$WAYLAND_DISPLAY" ]; do
 kill -0 "$compositor" 2>/dev/null || { echo WAYLAND_COMPOSITOR_EXITED; exit 1; }
 i=$((i+1)); [ "$i" -lt 100 ] || { echo WAYLAND_SOCKET_TIMEOUT; exit 1; }; sleep .1
done
timeout 30 "$work/client"
