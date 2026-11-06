#!/bin/sh
# Publish the compositor display for terminals and app launcher requests.
set -eu
session=${LR_DESKTOP_SESSION:-}
case "$session" in /home/mobile/Downloads/.linuxrunner/gui/session-????????-????-????-????-????????????) ;; *) exit 1;; esac
socket=${1:-${WAYLAND_DISPLAY:-}}
case "$socket" in /*) ;; *) socket="${XDG_RUNTIME_DIR:-}/$socket";; esac
[ -S "$socket" ] || exit 1
env_file="$HOME/.cache/linuxrunner/desktop.env"
[ -f "$env_file" ] && [ ! -L "$env_file" ] || exit 0
lock="$session/environment.lock"
i=0
until mkdir "$lock" 2>/dev/null; do
    [ ! -e "$session/stop" ] || exit 0
    i=$((i+1)); [ "$i" -lt 100 ] || exit 1; sleep .02
done
trap 'rmdir "$lock" 2>/dev/null || true' EXIT
# An older compositor must not replace a newer session's environment.
auth=$(sed -n 's/^XAUTHORITY=//p' "$env_file")
case "$auth" in "$session/authority"|"$session/xwayland-authority") ;; *) exit 0;; esac
display=$(sed -n 's/^DISPLAY=//p' "$env_file")
valid_display() {
    case "$1" in :*) number=${1#:};; *) return 1;; esac
    case "$number" in ''|*[!0-9]*) return 1;; esac
    [ -S "/tmp/.X11-unix/X$number" ]
}
# The compositor calls without arguments and supplies its own Xwayland display.
# The KMS bridge passes the Wayland socket and keeps the published X11 display.
auxiliary=$(cat "$session/display")
case "$auxiliary" in :*) ;; *) auxiliary=:$auxiliary;; esac
if [ "$#" = 0 ] && valid_display "${DISPLAY:-}" && [ "$DISPLAY" != "$auxiliary" ]; then
    display=$DISPLAY
    source_auth=${XAUTHORITY:-$session/authority}
    [ -r "$source_auth" ] || source_auth="$session/authority"
    cp "$source_auth" "$session/xwayland-authority.new"
    chmod 600 "$session/xwayland-authority.new"
    mv "$session/xwayland-authority.new" "$session/xwayland-authority"
    auth="$session/xwayland-authority"
fi
temporary="$env_file.$$.new"
{ grep -Ev '^(DISPLAY|XAUTHORITY|WAYLAND_DISPLAY)=' "$env_file" || true
  printf 'DISPLAY=%s\nXAUTHORITY=%s\nWAYLAND_DISPLAY=%s\n' "$display" "$auth" "$socket"
} > "$temporary"
chmod 600 "$temporary"
mv "$temporary" "$env_file"
echo "DESKTOP_STARTUP: compositor environment published DISPLAY=$display WAYLAND_DISPLAY=$socket"
