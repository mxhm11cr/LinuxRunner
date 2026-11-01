#!/bin/sh
set -eu
# Arguments identify an app-owned session directory, not arbitrary commands.
cd "$(dirname "$0")"
session=$1
mode=$2
case "$session" in session-????????-????-????-????-????????????|/home/mobile/Downloads/.linuxrunner/gui/session-????????-????-????-????-????????????) ;; *) echo 'Invalid session'; exit 2;; esac
case "$mode" in test|xterm|qt|custom|desktop) ;; *) exit 2;; esac
case "$session" in /*) ;; *) session="$PWD/$session";; esac
exec >> "$session/session.log" 2>&1
# New sessions and process-group signals: busybox where the distribution ships it
# (Debian, Alpine), otherwise util-linux setsid and the shell's kill builtin
# (Arch Linux ARM has no busybox).
if [ -x /bin/busybox ]; then setsid_cmd='/bin/busybox setsid'; group_kill() { /bin/busybox kill "$@"; }
elif command -v setsid >/dev/null; then setsid_cmd=setsid; group_kill() { kill "$@"; }
else echo 'GUI_SESSION: support missing: setsid (busybox or util-linux); run Install Desktop Support'; exit 7; fi
# The display and all its clients share the selected virtual Linux identity.
# Administration/bootstrap remains root; login is performed by the distro's
# actual su, including supplementary groups and HOME/USER/LOGNAME.
guest_user=root
[ ! -f "$session/guest-user" ] || guest_user=$(cat "$session/guest-user")
case "$guest_user" in ''|*[!a-z0-9_-]*|[0-9-]*) echo 'GUI_SESSION: invalid guest account'; exit 2;; esac
# Terminal login shells find the running desktop through this root-owned
# profile hook (desktop-env.sh parses and validates; it never evaluates).
if [ "$(id -u)" = 0 ] && [ -d /etc/profile.d ] && ! cmp -s ./desktop-env.sh /etc/profile.d/linuxrunner-desktop.sh; then
    if cp ./desktop-env.sh /etc/profile.d/linuxrunner-desktop.sh.new && chmod 644 /etc/profile.d/linuxrunner-desktop.sh.new &&
        mv /etc/profile.d/linuxrunner-desktop.sh.new /etc/profile.d/linuxrunner-desktop.sh; then echo 'GUI_SESSION: terminal_desktop_hook=installed'
    else rm -f /etc/profile.d/linuxrunner-desktop.sh.new; echo 'GUI_SESSION: terminal_desktop_hook=unavailable'; fi
fi
if [ "$(id -u)" = 0 ] && [ "$guest_user" != root ]; then
    entry=$(awk -F: -v name="$guest_user" '$1==name {print; count++} END {if(count!=1)exit 1}' /etc/passwd) || {
        echo 'GUI_SESSION: selected account missing; root fallback refused'; exit 10;
    }
    uid=$(printf '%s\n' "$entry" | cut -d: -f3)
    gid=$(printf '%s\n' "$entry" | cut -d: -f4)
    case "$uid:$gid" in *[!0-9:]*|0:*) echo 'GUI_SESSION: invalid non-root identity'; exit 10;; esac
    shell=$(printf '%s\n' "$entry" | cut -d: -f7)
    case "$shell" in */nologin|*/false|'') echo 'GUI_SESSION: account has no login shell'; exit 10;; esac
    # Only immutable support resources become readable. Authentication and
    # request files remain private to this selected user's owned session.
    chmod 711 .. .
    for resource in ./*; do
        [ -f "$resource" ] && [ ! -L "$resource" ] || continue
        case "$resource" in *.sh) chmod 755 "$resource";;
            *) if [ -x "$resource" ]; then chmod 755 "$resource"; else chmod 644 "$resource"; fi;;
        esac
    done
    [ ! -x ./window-observer ] || /bin/sh ./prepare-window-observer.sh || true
    # Hyprland draws on LinuxRunner's software KMS device: its nodes, a seat
    # (seatd without a virtual terminal) and udev's seat tag exist only for this
    # desktop; every other desktop runs without a DRM device, as before.
    # Wayfire uses it too, but only with the GPU (virtio-gpu card answering);
    # in software it stays nested in the X server as before.
    kms_desktop=$(cat "$session/desktop-environment" 2>/dev/null)
    if [ "$mode" = desktop ] && [ "$kms_desktop" = wayfire ] && command -v seatd >/dev/null; then
        mkdir -p /dev/dri
        [ -c /dev/dri/card1 ] || mknod -m 660 /dev/dri/card1 c 226 1
        if (exec 3<>/dev/dri/card1) 2>/dev/null; then kms_desktop=wayfire-gpu; else rm -f /dev/dri/card1; fi
    fi
    if [ "$mode" = desktop ] && { [ "$kms_desktop" = hyprland ] || [ "$kms_desktop" = wayfire-gpu ]; } && command -v seatd >/dev/null; then
        mkdir -p /dev/dri /run/udev/data
        [ -c /dev/dri/card0 ] || mknod -m 660 /dev/dri/card0 c 226 0
        [ -c /dev/dri/renderD128 ] || mknod -m 666 /dev/dri/renderD128 c 226 128
        chown "root:$gid" /dev/dri/card0
        # GPU acceleration (Settings › Graphics, virgl on Metal): its virtio-gpu
        # nodes stay only when the device answers; otherwise software KMS only.
        [ -c /dev/dri/card1 ] || mknod -m 660 /dev/dri/card1 c 226 1
        [ -c /dev/dri/renderD129 ] || mknod -m 666 /dev/dri/renderD129 c 226 129
        if (exec 3<>/dev/dri/card1) 2>/dev/null; then chown "root:$gid" /dev/dri/card1; echo "GUI_SESSION: gpu=virgl-metal"
        else rm -f /dev/dri/card1 /dev/dri/renderD129; fi
        for minor in 0 1 128 129; do printf 'E:ID_SEAT=seat0\nG:seat\nG:master-of-seat\n' > "/run/udev/data/c226:$minor"; done
        if ! [ -S /run/seatd.sock ] || ! pgrep -x seatd >/dev/null 2>&1; then
            rm -f /run/seatd.sock
            SEATD_VTBOUND=0 seatd -u "$guest_user" -g "$(id -gn "$guest_user")" > "$session/seatd.log" 2>&1 &
        fi
        echo "GUI_SESSION: software_kms=prepared seat=seatd"
    elif [ "$mode" = desktop ] && [ -c /dev/dri/card0 ]; then
        rm -f /dev/dri/card0 /dev/dri/renderD128 /dev/dri/card1 /dev/dri/renderD129
    fi
    chown -R "$uid:$gid" "$session"
    absolute_session="$session"
    case "$PWD" in /home/mobile/Downloads/.linuxrunner/gui) ;; *) echo 'GUI_SESSION: unexpected support path'; exit 10;; esac
    echo "GUI_SESSION: login user=$guest_user uid=$uid gid=$gid"
    exec su -l -s /bin/sh -c "exec /bin/sh '$PWD/session.sh' '$absolute_session' '$mode'" "$guest_user"
fi
if [ "$guest_user" != root ] && [ "$(id -un)" != "$guest_user" ]; then
    echo 'GUI_SESSION: selected identity mismatch'; exit 10
fi
echo "GUI_SESSION: effective_uid=$(id -u) effective_gid=$(id -g) user=$(id -un) home=$HOME"
# Read-only metadata evidence for packaged Chromium/Electron sandbox helpers.
# Never grant set-ID privileges or disable sandboxing from a launcher.
helper_count=0
for helper in /usr/share/*/chrome-sandbox /usr/lib/*/chrome-sandbox /opt/*/*/chrome-sandbox; do
    [ -f "$helper" ] || continue
    helper_count=$((helper_count + 1))
    [ "$helper_count" -le 32 ] || break
    stat -c 'SANDBOX_HELPER: mode=%a uid=%u gid=%g path=%n' "$helper" || true
done

xpid= vpid= wpid= mpid= bpid= apid= spid= appgroup= runtime_dir= desktop_env=
cleanup() {
    trap - EXIT HUP INT TERM
    touch "$session/stop"
    # Generic custom apps get a separate guest session/process group. Never
    # signal the CLI's group or guest init; children retain the owned group ID.
    if [ -n "$appgroup" ] && [ "$appgroup" -gt 1 ] && [ "$(cat "$session/application-group" 2>/dev/null || true)" = "$appgroup" ]; then
        group_kill -TERM "-$appgroup" 2>/dev/null || true
    fi
    if [ "$mode" = desktop ] && [ -n "$bpid" ] && [ "$(cat "$session/bus-group" 2>/dev/null || true)" = "$bpid" ]; then
        group_kill -TERM "-$bpid" 2>/dev/null || true
    fi
    if [ -n "$spid" ] && [ "$(cat "$session/audio-group" 2>/dev/null || true)" = "$spid" ]; then
        group_kill -TERM "-$spid" 2>/dev/null || true
    fi
    for pid in "$apid" "$vpid" "$wpid" "$mpid" "$spid" "$bpid" "$xpid"; do [ -z "$pid" ] || kill "$pid" 2>/dev/null || true; done
    # A client may be blocked in X I/O while the display disappears. Bound
    # graceful shutdown, then stop remaining owned children. Check their
    # parent before escalation; do not signal another session on PID reuse.
    i=0
    while [ "$i" -lt 20 ]; do
        live=false
        for pid in "$apid" "$vpid" "$wpid" "$mpid" "$spid" "$bpid" "$xpid"; do
            [ -z "$pid" ] || ! kill -0 "$pid" 2>/dev/null || {
                state=$(sed 's/.*) //' "/proc/$pid/stat" 2>/dev/null | cut -d ' ' -f 1)
                [ "$state" = Z ] || live=true
            }
        done
        [ "$live" = true ] || break
        i=$((i+1)); sleep .1
    done
    if [ -n "$appgroup" ] && [ "$appgroup" -gt 1 ] && [ "$(cat "$session/application-group" 2>/dev/null || true)" = "$appgroup" ]; then
        group_kill -KILL "-$appgroup" 2>/dev/null || true
        echo "GUI_SESSION: stopped owned application process group=$appgroup"
    fi
    if [ "$mode" = desktop ] && [ -n "$bpid" ] && [ "$(cat "$session/bus-group" 2>/dev/null || true)" = "$bpid" ]; then
        group_kill -KILL "-$bpid" 2>/dev/null || true
    fi
    if [ -n "$spid" ] && [ "$(cat "$session/audio-group" 2>/dev/null || true)" = "$spid" ]; then
        group_kill -KILL "-$spid" 2>/dev/null || true
    fi
    for pid in "$apid" "$vpid" "$wpid" "$mpid" "$spid" "$bpid" "$xpid"; do
        [ -n "$pid" ] || continue
        owner=$(sed 's/.*) //' "/proc/$pid/stat" 2>/dev/null | cut -d ' ' -f 2)
        [ "$owner" != "$$" ] || kill -KILL "$pid" 2>/dev/null || true
    done
    for pid in "$apid" "$vpid" "$wpid" "$mpid" "$spid" "$bpid" "$xpid"; do [ -z "$pid" ] || wait "$pid" 2>/dev/null || true; done
    # The terminal hook's record of this desktop, only while it names this display.
    if [ -n "$desktop_env" ] && grep -qx "DISPLAY=${DISPLAY:-none}" "$desktop_env" 2>/dev/null; then rm -f "$desktop_env"; fi
    # Only our uniquely created runtime directory; preserve user profiles.
    [ -z "$runtime_dir" ] || rm -rf "$runtime_dir"
    rm -f "$session/password" "$session/cookie" "$session/authority"
    echo 'GUI_SESSION: cleaned up display children; CLI runtime retained'
}
trap cleanup EXIT HUP INT TERM
[ -x ./focus-manager ] && command -v Xvfb >/dev/null && command -v x11vnc >/dev/null || { echo 'GUI_SESSION: support missing; run Install GUI Support'; exit 3; }
# Refresh only the optional diagnostic helper on source changes, using the
# existing GUI compiler. Failure must not interrupt the proven display path.
observer_ready=no
if [ -x ./window-observer ] && [ "$(id -u)" != 0 ]; then observer_ready=yes
elif /bin/sh ./prepare-window-observer.sh; then observer_ready=yes
else echo 'X11_STARTUP: observer_unavailable (display/application launch continues)'; fi
port=$(cat "$session/port")
display=$(cat "$session/display")
case "$port:$display" in *[!0-9:]*|:*) exit 2;; esac
export DISPLAY=:$display
# Debian Mesa needs server GLX for real desktop OpenGL contexts. This is
# software rendering, independent of optional Metal framebuffer presentation.
distro=$(sed -n 's/^ID=//p' /etc/os-release | tr -d '"')
# Session graphics default chosen in Settings; a launcher's own
# LR_GUI_GPU_MODE (command.sh) takes precedence for that application.
gpu_mode=mesa
[ ! -f "$session/gpu-mode" ] || gpu_mode=$(cat "$session/gpu-mode")
case "$gpu_mode" in mesa|mesa-metal-resident|mesa-metal-verify) ;; *) gpu_mode=mesa;; esac
export LR_GUI_GPU_MODE="$gpu_mode" LR_GUI_SESSION_DIR="$session"
# Desktop environment chosen in Settings › Desktop (desktop mode): linuxrunner or plasma (experimental).
desktop_environment=linuxrunner
[ ! -f "$session/desktop-environment" ] || desktop_environment=$(cat "$session/desktop-environment")
case "$desktop_environment" in linuxrunner|plasma|lxqt|enlightenment|wayfire|hyprland) ;; *) desktop_environment=linuxrunner;; esac
export LR_DESKTOP_ENVIRONMENT="$desktop_environment"
glx_mode=off
[ "$distro" != debian ] || glx_mode=software
[ ! -f "$session/glx-mode" ] || glx_mode=$(cat "$session/glx-mode")
case "$glx_mode" in
 off) glx_switch=-extension;;
 software) glx_switch=+extension;;
 *) echo 'GUI_SESSION: invalid GLX mode (off or software required)'; exit 2;;
esac
echo "GUI_GRAPHICS: GLX=$glx_mode application-renderer=selected-by-guest-library final-presentation=host"
# Keep the established socket-image path as rollback. Shared images are a
# session-scoped experiment until real client and IPC lifetime tests pass.
shm_mode=off
[ ! -f "$session/shm-mode" ] || shm_mode=$(cat "$session/shm-mode")
case "$shm_mode" in
 off) shm_switch=-extension; vnc_shm_option=-noshm;;
 sysv) shm_switch=+extension; vnc_shm_option=;;
 *) echo 'GUI_SESSION: invalid shared-image mode (off or sysv required)'; exit 2;;
esac
echo "GUI_CAPTURE: shared-images=$shm_mode"
# Wayfire draws into this X server through MIT-SHM (wlroots' X11 backend with its
# software renderer); x11vnc keeps the capture path chosen above.
[ "$mode" != desktop ] || [ "$desktop_environment" != wayfire ] || shm_switch=+extension
export XDG_SESSION_TYPE=x11
export QT_QPA_PLATFORM=${QT_QPA_PLATFORM:-xcb}
export QT_FORCE_STDERR_LOGGING=${QT_FORCE_STDERR_LOGGING:-1}
export QT_WIDGETS_RHI=${QT_WIDGETS_RHI:-0} QT_QUICK_BACKEND=${QT_QUICK_BACKEND:-software}
export QT_OPENGL=${QT_OPENGL:-software} LIBGL_ALWAYS_SOFTWARE=${LIBGL_ALWAYS_SOFTWARE:-1}
# A fresh X server belongs to this guest root; DISPLAY is session-scoped.
width=640 height=480
if [ -f "$session/geometry" ]; then
    geometry=$(cat "$session/geometry")
    # A preset or the app's "Fit Screen" size: even WxH within 320x240..2560x1600.
    case "$geometry" in ''|*[!0-9x]*|x*|*x|*x*x*) echo 'GUI_SESSION: invalid display geometry'; exit 2;; esac
    width=${geometry%x*};height=${geometry#*x}
    if [ "$width" -lt 320 ] || [ "$width" -gt 2560 ] || [ "$height" -lt 240 ] || [ "$height" -gt 1600 ] || [ $((width % 2)) -ne 0 ] || [ $((height % 2)) -ne 0 ]; then echo 'GUI_SESSION: invalid display geometry'; exit 2; fi
fi
export XAUTHORITY="$session/authority"
umask 077
xauth -f "$XAUTHORITY" add "$DISPLAY" . "$(cat "$session/cookie")"
echo "GUI_SESSION: root=${LINUXRUNNER_ENVIRONMENT:-current} distribution=$distro DISPLAY=$DISPLAY geometry=${width}x${height} backend=xvfb-rfb interpreter-baseline"
echo "GUI_SESSION: starting X.Org software display, ${width}x${height}, no JIT"
Xvfb "$DISPLAY" -screen 0 "${width}x${height}x24" -nolisten tcp -auth "$XAUTHORITY" "$shm_switch" MIT-SHM "$glx_switch" GLX > "$session/xvfb.log" 2>&1 & xpid=$!
i=0
while [ ! -S "/tmp/.X11-unix/X$display" ]; do
    [ ! -f "$session/stop" ] || exit 0
    kill -0 "$xpid" 2>/dev/null || { echo 'GUI_SESSION: X server exited'; cat "$session/xvfb.log"; exit 4; }
    i=$((i+1)); [ "$i" -lt 100 ] || { echo 'GUI_SESSION: X server timeout'; exit 5; }; sleep .1
done
echo "GUI_SESSION: stage=x11-ready socket=/tmp/.X11-unix/X$display polls=$i"
if [ "$mode" != desktop ]; then
# Session-wide focus policy for all X11 clients; no app-name/key special cases.
./focus-manager "$session/stop" > "$session/focus.log" 2>&1 & wpid=$!
i=0
# Up to 15 s: the focus manager retries while Xvfb starts listening.
while ! grep -q 'FOCUS: ready' "$session/focus.log"; do
    kill -0 "$wpid" 2>/dev/null || { echo 'GUI_SESSION: focus manager failed'; cat "$session/focus.log"; exit 6; }
    [ ! -f "$session/stop" ] || exit 0
    i=$((i+1)); [ "$i" -lt 300 ] || { echo 'GUI_SESSION: focus manager timeout'; exit 6; }; sleep .05
done
fi
if [ "$observer_ready" = yes ]; then
    ./window-observer 86400 "$session/window-state" "$session/stop" > "$session/window.log" 2>&1 & mpid=$!
fi
setxkbmap -query > "$session/keymap.log" 2>&1 || true
xmodmap -pm >> "$session/keymap.log" 2>&1 || true
xkb=-xkb
if [ -f "$session/xkb-mode" ] && [ "$(cat "$session/xkb-mode")" = noxkb ]; then xkb=-noxkb; fi
focus_policy=managed; [ "$mode" != desktop ] || focus_policy=window-manager
echo "GUI_SESSION: keyboard=$xkb focus=$focus_policy encoding=Hextile/Raw software-rendering"
# A Linux sockaddr_un path holds at most 107 bytes. A support-directory UUID
# path plus GVFS/Qt socket suffix exceeds it. Use a uniquely owned short path
# for every GUI mode, with restrictive permissions and no preexisting reuse.
runtime_dir=$(mktemp -d /tmp/lr-runtime.XXXXXXXXXX) || { echo 'GUI_SESSION: runtime-directory-create-failed'; exit 9; }
chmod 700 "$runtime_dir"
export XDG_RUNTIME_DIR="$runtime_dir"
echo "GUI_SESSION: runtime_directory=$XDG_RUNTIME_DIR (short Unix socket namespace)"
export SHELL="${SHELL:-/bin/sh}" LANG="${LANG:-C.UTF-8}"
if [ "$mode" = desktop ]; then export XDG_CURRENT_DESKTOP=LXDE DESKTOP_SESSION=LinuxRunner; fi
# Reusable session bus, when installed. This is not a systemd/system bus.
# Keep it foreground in an owned child so display shutdown also stops it.
if command -v dbus-daemon >/dev/null; then
    if [ "$mode" = desktop ]; then
        $setsid_cmd /bin/sh -c 'echo $$ > "$1/bus-group"; exec dbus-daemon --session --nofork --print-address' sh "$session" > "$session/bus.address" 2> "$session/bus.log" & bpid=$!
    else dbus-daemon --session --nofork --print-address > "$session/bus.address" 2> "$session/bus.log" & bpid=$!; fi
    i=0
    while [ ! -s "$session/bus.address" ] && kill -0 "$bpid" 2>/dev/null; do
        i=$((i+1)); [ "$i" -lt 100 ] || break; sleep .02
    done
    address=$(head -n 1 "$session/bus.address" 2>/dev/null || true)
    case "$address" in unix:*) export DBUS_SESSION_BUS_ADDRESS="$address"; echo 'GUI_SESSION: dbus_session=ready'
        if command -v dbus-update-activation-environment >/dev/null; then
            if dbus-update-activation-environment DISPLAY XAUTHORITY XDG_RUNTIME_DIR HOME PATH DBUS_SESSION_BUS_ADDRESS; then
                echo 'GUI_SESSION: dbus_activation_environment=PASS (explicit session variables, no systemd required)'
            else echo 'GUI_SESSION: dbus_activation_environment=FAIL; inspect bus.log'; fi
        fi;;
        *) echo 'GUI_SESSION: dbus_session=unavailable; inspect bus.log (applications still attempted)';;
    esac
else echo 'GUI_SESSION: dbus_session=not-installed'; fi
# Linux sound (desktop and app sessions): audio-session.sh runs PulseAudio,
# a low-latency recorder of its output and audio-relay, which offers the PCM
# only on the loopback port the app chose. Own process group, stopped below.
audio_port=
[ ! -s "$session/audio-port" ] || audio_port=$(cat "$session/audio-port")
case "$audio_port" in *[!0-9]*) audio_port=;; esac
if [ -n "$audio_port" ] && command -v pulseaudio >/dev/null && command -v parec >/dev/null && [ -x ./audio-relay ]; then
    $setsid_cmd /bin/sh ./audio-session.sh "$session" "$audio_port" > "$session/audio.log" 2>&1 & spid=$!
    echo "GUI_SESSION: audio=pulseaudio group=$spid"
elif [ -n "$audio_port" ]; then echo 'GUI_SESSION: audio=unavailable (Desktop > Install Desktop Support adds sound)'; fi
# Terminals of the same account join this desktop (DISPLAY, X authority,
# runtime directory, session bus); see desktop-env.sh. Private to the account.
if [ "$mode" = desktop ]; then
    desktop_env="$HOME/.cache/linuxrunner/desktop.env"
    if mkdir -p "$HOME/.cache/linuxrunner" && chmod 700 "$HOME/.cache/linuxrunner" &&
        ( umask 077; printf 'DISPLAY=%s\nXAUTHORITY=%s\nXDG_RUNTIME_DIR=%s\n' "$DISPLAY" "$XAUTHORITY" "$XDG_RUNTIME_DIR"
          [ -z "${DBUS_SESSION_BUS_ADDRESS:-}" ] || printf 'DBUS_SESSION_BUS_ADDRESS=%s\n' "$DBUS_SESSION_BUS_ADDRESS" ) > "$desktop_env.new" &&
        mv "$desktop_env.new" "$desktop_env"; then echo 'GUI_SESSION: terminal_desktop_environment=published'
    else rm -f "$desktop_env.new"; desktop_env=; echo 'GUI_SESSION: terminal_desktop_environment=unavailable'; fi
fi
# Only local authenticated RFB; ordinary X image operations, no guest/native SHM.
# Experimental input policy is session-scoped; legacy is the rollback/default.
input_mode=legacy
[ ! -f "$session/input-mode" ] || input_mode=$(cat "$session/input-mode")
input_option=
case "$input_mode" in
 legacy) ;;
 eager) input_option=-input_eagerly;;
 *) echo 'GUI_SESSION: invalid input mode (legacy or eager required)'; exit 2;;
esac
echo "GUI_INPUT: mode=$input_mode experimental=$([ "$input_mode" = eager ] && echo yes || echo no)"
# x11vnc update policy. default: poll every 25 ms, defer updates 16 ms, no
# XDAMAGE. threads (the app's "Low-latency input" switch) services client input
# on its own thread: Mac fixture click-to-repaint median 108 -> 50 ms, idle CPU
# +8%, keystrokes exact, but keys arriving one by one let Brave's omnibox
# autocomplete splice typed URLs (2 of 5 gate runs), so it stays opt-in.
# The others are latency experiments.
vnc_timing=default
[ ! -f "$session/vnc-timing" ] || vnc_timing=$(cat "$session/vnc-timing")
case "$vnc_timing" in
 default|legacy) vnc_timing_options="-noxdamage -defer 16 -wait 25";;
 threads) vnc_timing_options="-noxdamage -defer 16 -wait 25 -threads";;
 fast) vnc_timing_options="-noxdamage -defer 5 -wait 10";;
 damage) vnc_timing_options="-defer 16 -wait 25";;
 fast-damage) vnc_timing_options="-defer 5 -wait 10";;
 # Poll at the idle rate, but -wait_ui divides the interval while input is
 # recent: interactive latency of "fast" without its idle polling cost.
 adaptive) vnc_timing_options="-noxdamage -defer 5 -wait 25 -wait_ui 2.5";;
 adaptive-slow) vnc_timing_options="-noxdamage -defer 5 -wait 40 -wait_ui 4";;
 balanced) vnc_timing_options="-noxdamage -defer 5 -wait 15";;
 threads-balanced) vnc_timing_options="-noxdamage -defer 5 -wait 15 -threads";;
 *) echo 'GUI_SESSION: invalid vnc timing'; exit 2;;
esac
echo "GUI_VNC: timing=$vnc_timing options=$vnc_timing_options"
vnc_lifetime="-once -timeout 60"
[ "$mode" != desktop ] || vnc_lifetime="-forever"
# KMS compositors (Hyprland; Wayfire on the GPU) draw their own pointer into
# the output the bridge shows: x11vnc must not paint a second one.
vnc_cursor_option=
case "$mode:$(cat "$session/desktop-environment" 2>/dev/null)" in
    desktop:hyprland) if command -v Hyprland >/dev/null && [ -c /dev/dri/card0 ]; then vnc_cursor_option=-nocursor; fi;;
    desktop:wayfire) if [ -c /dev/dri/card1 ] && command -v wayfire >/dev/null; then vnc_cursor_option=-nocursor; fi;;
esac
[ -z "$vnc_cursor_option" ] || echo 'GUI_SESSION: pointer=compositor (x11vnc draws none)'
x11vnc -norc -display "$DISPLAY" -auth "$XAUTHORITY" -localhost -no6 -rfbport "$port" -passwdfile "$session/password" $vnc_lifetime $vnc_shm_option $vnc_timing_options -noxrecord -noscr -nosel -noclipboard -nocursorshape $vnc_cursor_option -nolookup $input_option "$xkb" > "$session/display.log" 2>&1 & vpid=$!
echo "GUI_SESSION: stage=vnc-started port=$port"
if [ "$mode" = test ]; then ./gui-test > "$session/application.log" 2>&1 &
elif [ "$mode" = xterm ]; then xterm -geometry 80x24+0+0 -fn fixed -xrm 'XTerm*renderFont: false' > "$session/application.log" 2>&1 &
elif [ "$mode" = qt ]; then
    [ -x ./qt-test ] || { echo 'GUI_SESSION: Qt support missing; run Install Qt Support'; exit 7; }
    QT_QPA_PLATFORM=xcb ./qt-test > "$session/application.log" 2>&1 &
elif [ "$mode" = desktop ]; then
    # Desktop components and everything launched from the desktop inherit the
    # graphics routing; Xvfb and x11vnc above were started without it.
    ( . ./gpu-env.sh; lr_gpu_route "$LR_GUI_GPU_MODE" "$PWD" "$session/gpu-stats"
      exec $setsid_cmd /bin/sh -c 'echo $$ > "$1/application-group"; exec /bin/sh ./desktop-components.sh "$1"' sh "$session" ) > "$session/application.log" 2>&1 &
else
    [ -f "$session/command.sh" ] || { echo 'GUI_SESSION: launch configuration missing'; exit 8; }
    $setsid_cmd /bin/sh -c 'echo $$ > "$1/application-group"; exec /bin/sh "$1/command.sh"' sh "$session" > "$session/application.log" 2>&1 &
fi
apid=$!
case "$mode" in custom|desktop) appgroup=$apid;; esac
echo "GUI_SESSION: application started mode=$mode"
# A launcher may hand off to a child and exit successfully. Keep its owned
# process group, rather than its initial PID, as the custom session lifetime.
application_live() {
    if [ -n "$apid" ]; then
        kill -0 "$apid" 2>/dev/null && return 0
        code=0; wait "$apid" || code=$?; apid=
        echo "GUI_SESSION: application leader exited status=$code (128+ may indicate signal; see application.log)"
    fi
    [ -n "$appgroup" ] && [ "$appgroup" -gt 1 ] || return 1
    [ "$(cat "$session/application-group" 2>/dev/null || true)" = "$appgroup" ] || return 1
    group_kill -0 "-$appgroup" 2>/dev/null || return 1
    # Exclude zombies; a terminated orphan must not keep the viewer alive.
    ps -o pgid,stat | awk -v group="$appgroup" '$1 == group && $2 !~ /^Z/ { alive=1 } END { exit !alive }'
}
# Disconnect or the last owned app process exiting tears down this session.
while [ ! -f "$session/stop" ] && application_live && kill -0 "$vpid" 2>/dev/null && { [ "$mode" = desktop ] || kill -0 "$wpid" 2>/dev/null; }; do sleep .2; done
if application_live; then echo 'GUI_SESSION: display disconnected'
else echo 'GUI_SESSION: application process group ended'; fi
