#!/bin/sh
# Arguments are a validated directory, executable, then literal argv. No eval.
set -eu
cwd=$1; command=$2; shift 2
# Resolve before changing directories: command-line callers can use a relative
# path to this helper, while the application's managed launcher uses an absolute one.
base=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
# Existing launcher presets used /root as the default login directory. Keep
# those presets usable after selecting a normal guest account; profiles remain
# in that account's real HOME and the former root data is not migrated.
if [ "$cwd" = /root ] && [ "$(id -u)" != 0 ]; then
    echo "GUI_LAUNCH: legacy-login-directory=/root selected-home=$HOME"
    cwd=$HOME
fi
cd "$cwd" || { echo "GUI_LAUNCH: working directory missing: $cwd"; exit 125; }
[ -f "$command" ] || { echo "GUI_LAUNCH: executable missing: $command"; exit 127; }
[ -x "$command" ] || { echo "GUI_LAUNCH: executable not executable: $command"; exit 126; }
if command -v readelf >/dev/null; then
    loader=$(readelf -l "$command" 2>/dev/null | sed -n 's/.*interpreter: \(.*\)\]/\1/p') || true
    [ -z "$loader" ] || [ -x "$loader" ] || { echo "GUI_LAUNCH: dynamic loader missing: $loader"; exit 126; }
fi
# Independent launcher calls can adopt the validated active account session.
# Session-owned requests already inherit its exact DISPLAY/auth/bus/runtime.
if [ -z "${DISPLAY:-}" ] && [ -z "${WAYLAND_DISPLAY:-}" ]; then . "$base/desktop-env.sh"; fi
[ -n "${DISPLAY:-}" ] || [ -n "${WAYLAND_DISPLAY:-}" ] || { echo 'GUI_LAUNCH: no active display for this account; open Desktop first'; exit 125; }
# Hyprland or Wayfire running: open inside it (its Wayland socket), not under it.
[ -n "${WAYLAND_DISPLAY:-}" ] || { command -v linuxrunner_wayland >/dev/null 2>&1 || . "$base/desktop-env.sh"; linuxrunner_wayland || true; }
export QT_FORCE_STDERR_LOGGING=${QT_FORCE_STDERR_LOGGING:-1}
echo "GUI_LAUNCH: executable=$command directory=$PWD DISPLAY=${DISPLAY:-none} WAYLAND_DISPLAY=${WAYLAND_DISPLAY:-none}"
# Graphics routing: the launcher's own choice, else the session default from
# Settings (session.sh exports it). Unusable GPU modes fall back to Mesa.
. "$base/gpu-env.sh"
lr_gpu_route "${LR_GUI_GPU_MODE:-mesa}" "$base" "${LR_GUI_SESSION_DIR:+$LR_GUI_SESSION_DIR/gpu-stats}"
exec "$command" "$@"
