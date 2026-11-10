#!/bin/sh
# Managed Plasma Wayland session. The existing native RFB viewer is the output.
set -eu
base=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
session=$1
role=${2:-outer}
case "$session" in /home/mobile/Downloads/.linuxrunner/gui/session-????????-????-????-????-????????????) ;; *) echo 'PLASMA_SESSION: invalid managed session'; exit 2;; esac
case "$role" in outer|group|bus|components) ;; *) exit 2;; esac
# Startup timeline (guest monotonic clock, 10 ms): PLASMA_STARTUP: stage=<name> at_ms=<uptime>
stage() { printf 'PLASMA_STARTUP: stage=%s at_ms=%s\n' "$1" "$(awk '{printf "%d", $1*1000}' /proc/uptime 2>/dev/null)"; }
[ "$(id -u)" != 0 ] || { echo 'PLASMA_SESSION: choose a regular Linux user in Settings before starting Plasma'; exit 3; }
[ -d "$session" ] && [ ! -L "$session" ] && [ -O "$session" ] || exit 3
# The support tree is intentionally root-owned 0711. Applications must inherit
# the regular user's readable home, not that private support directory (KDE's
# file picker prefers CWD and rejects an unreadable starting directory).
cd "$HOME"
if [ "$role" = group ]; then
 echo $$ > "$session/application-group"
 exec dbus-run-session -- /bin/sh "$base/plasma-session.sh" "$session" bus
fi
if [ "$role" = bus ]; then
 audio_port=$(cat "$session/audio-port" 2>/dev/null || true)
 case "$audio_port" in ''|*[!0-9]*) audio_port=;; esac
 if [ -n "$audio_port" ] && command -v pulseaudio >/dev/null && command -v parec >/dev/null && [ -x "$base/audio-relay" ]; then
  /bin/sh "$base/audio-session.sh" "$session" "$audio_port" inherited-group > "$session/audio.log" 2>&1 &
  echo 'PLASMA_SESSION: existing audio relay started in the owned session group'
 elif [ -n "$audio_port" ]; then echo 'PLASMA_SESSION: optional audio support is not installed'; fi
 pipewire > "$session/pipewire.log" 2>&1 &
 wireplumber > "$session/wireplumber.log" 2>&1 &
 echo $$ > "$session/kwin-pid"
 stage kwin-exec
 exec kwin_wayland --virtual --width "$LR_PLASMA_WIDTH" --height "$LR_PLASMA_HEIGHT" --socket wayland-0 --no-lockscreen --exit-with-session "$base/plasma-session.sh '$session' components" > "$session/wm.log" 2>&1
fi
if [ "$role" = components ]; then
 # Keep applications on the established software path by default. The
 # compositor/capture process retain their separately selected render node.
 export LIBGL_ALWAYS_SOFTWARE=1 QT_OPENGL=software
 dbus-update-activation-environment --all > "$session/bus.log" 2>&1
 stage components
 kded6 > "$session/kded.log" 2>&1 &
 ksmserver > "$session/ksmserver.log" 2>&1 &
 plasmashell > "$session/plasma.log" 2>&1 & shell=$!
 bus_owner() { gdbus call --session --dest org.freedesktop.DBus --object-path /org/freedesktop/DBus --method org.freedesktop.DBus.NameHasOwner "$1" 2>/dev/null | grep -q true; }
 i=0
 until bus_owner org.kde.plasmashell; do
  kill -0 "$shell" 2>/dev/null || { echo 'PLASMA_SESSION: Plasma shell exited'; exit 4; }
  [ ! -e "$session/stop" ] || exit 0
  i=$((i+1)); [ "$i" -lt 120 ] || { echo 'PLASMA_SESSION: Plasma bus startup deadline exceeded'; exit 4; }; sleep 1
 done
 stage plasmashell-on-bus
 # This desktop entry identifies the app-owned capture process to KWin.
 data=${XDG_DATA_HOME:-$HOME/.local/share}
 mkdir -p "$data/applications"
 cat > "$data/applications/org.linuxrunner.plasma-bridge.desktop.new" <<DESKTOP
[Desktop Entry]
Type=Application
Name=LinuxRunner Plasma viewer
Exec=$base/lr-plasma-bridge
NoDisplay=true
X-KDE-Wayland-Interfaces=zkde_screencast_unstable_v1
DESKTOP
 mv "$data/applications/org.linuxrunner.plasma-bridge.desktop.new" "$data/applications/org.linuxrunner.plasma-bridge.desktop"
 LIBGL_ALWAYS_SOFTWARE="$LR_PLASMA_SOFTWARE" "$base/lr-plasma-bridge" --name LinuxRunner --resolution "${LR_PLASMA_WIDTH}x${LR_PLASMA_HEIGHT}" --port "$(cat "$session/port")" --password-file "$session/password" > "$session/display.log" 2>&1 & bridge=$!
 trap 'kill "$bridge" "$shell" 2>/dev/null || true' EXIT
 i=0
 until kscreen-doctor -o 2>/dev/null | grep -q Virtual-LinuxRunner; do
  kill -0 "$bridge" 2>/dev/null || { echo 'PLASMA_SESSION: display bridge exited'; exit 5; }
  i=$((i+1)); [ "$i" -lt 60 ] || { echo 'PLASMA_SESSION: virtual monitor startup deadline exceeded'; exit 5; }; sleep 1
 done
 stage virtual-output
 kscreen-doctor output.Virtual-LinuxRunner.primary output.Virtual-0.disable >> "$session/plasma.log" 2>&1
 # Pure Wayland state, parsed by desktop-env.sh; never publish a fake DISPLAY.
 envdir=$HOME/.cache/linuxrunner
 mkdir -p "$envdir"
 umask 077
 { printf 'XDG_RUNTIME_DIR=%s\n' "$XDG_RUNTIME_DIR"; printf 'DBUS_SESSION_BUS_ADDRESS=%s\n' "$DBUS_SESSION_BUS_ADDRESS"; printf 'WAYLAND_DISPLAY=%s/%s\n' "$XDG_RUNTIME_DIR" "$WAYLAND_DISPLAY"; } > "$envdir/desktop.env.new"
 mv "$envdir/desktop.env.new" "$envdir/desktop.env"
 mkdir -p "$session/requests"
 i=0 ready=0
 while [ ! -e "$session/stop" ]; do
  kill -0 "$bridge" 2>/dev/null && kill -0 "$shell" 2>/dev/null || { echo 'PLASMA_SESSION: required desktop component exited'; exit 6; }
  if [ "$ready" = 0 ] && grep -q 'PLASMA_BRIDGE: frame-presented' "$session/display.log" && grep -q 'LR_KWIN_EIS: absolute pointer ready' "$session/display.log" && grep -q 'LR_KWIN_EIS: keysym input ready' "$session/display.log"; then
   printf 'environment=plasma\ndisplay_server=wayland\nwm=1\npanel=1\nsurface=1\nready=1\nroot_geometry=%sx%s\n' "$LR_PLASMA_WIDTH" "$LR_PLASMA_HEIGHT" > "$session/desktop-state.new"
   mv "$session/desktop-state.new" "$session/desktop-state"
   ready=1; stage ready; echo 'PLASMA_SESSION: session, frame transport and input initialized'
   # Check KWin itself after the first captured frame. A render node being
   # available alone does not tell us whether KWin actually used virgl.
   (
    info=$(gdbus call --session --timeout 4 --dest org.kde.KWin --object-path /KWin --method org.kde.KWin.supportInformation 2>/dev/null) || exit 0
    renderer=$(printf '%s\n' "$info" | sed -n 's/.*OpenGL renderer string: *\([^\\]*\).*/\1/p')
    [ -z "$renderer" ] || printf 'PLASMA_SESSION: compositor_renderer=%s\n' "$renderer"
   ) &
  fi
  i=$((i+1)); [ "$ready" = 1 ] || [ "$i" -lt 240 ] || { echo 'PLASMA_SESSION: frame/input initialization deadline exceeded'; exit 6; }
  for request in "$session"/requests/launch-*.sh; do
   [ -f "$request" ] && [ ! -L "$request" ] || continue
   name=${request##*/}; mv "$request" "$request.running"
   (code=0; /bin/sh "$request.running" >> "$session/application.log" 2>&1 || code=$?; printf '%s\n' "$code" > "$session/requests/$name.result"; rm -f "$request.running") &
  done
  sleep .5
 done
 exit 0
fi
for tool in kwin_wayland plasmashell kded6 ksmserver pipewire wireplumber dbus-run-session gdbus kscreen-doctor setsid xdg-user-dirs-update; do
 command -v "$tool" >/dev/null || { echo "PLASMA_SESSION: missing=$tool; install Plasma support"; exit 3; }
done
/bin/sh "$base/desktop-user-dirs.sh" > "$session/user-dirs.log" 2>&1 || { cat "$session/user-dirs.log"; exit 3; }
[ -x "$base/lr-plasma-bridge" ] || { echo 'PLASMA_SESSION: display bridge missing; install Plasma support'; exit 3; }
geometry=$(cat "$session/geometry")
case "$geometry" in ''|*[!0-9x]*|x*|*x|*x*x*) exit 2;; esac
LR_PLASMA_WIDTH=${geometry%x*}; LR_PLASMA_HEIGHT=${geometry#*x}
[ "$LR_PLASMA_WIDTH" -ge "$LR_PLASMA_HEIGHT" ] && [ "$LR_PLASMA_WIDTH" -ge 320 ] && [ "$LR_PLASMA_WIDTH" -le 2560 ] && [ "$LR_PLASMA_HEIGHT" -ge 240 ] && [ "$LR_PLASMA_HEIGHT" -le 1600 ] || exit 2
runtime_dir=$(mktemp -d /tmp/lr-runtime.XXXXXXXXXX)
chmod 700 "$runtime_dir"
export XDG_RUNTIME_DIR="$runtime_dir" LR_PLASMA_WIDTH LR_PLASMA_HEIGHT
# Responsiveness defaults (lowest priority, XDG_CONFIG_DIRS): every frame is
# software-rendered through the interpreter, so animations, blur/translucency,
# shadows, file indexing and optional background services cost visible
# latency. Anything the user changes in System Settings (~/.config) wins.
defaults_dir="$runtime_dir/config-defaults"
mkdir -p "$defaults_dir"
cat > "$defaults_dir/kdeglobals" <<'CONF'
[KDE]
AnimationDurationFactor=0
CONF
{
 echo '[Plugins]'
 for effect in blur contrast backgroundcontrast slidingpopups fadingpopups scale fade slide squash maximize magiclamp login logout startupfeedback windowaperture dialogparent frozenapp screenedge kscreen fullscreen outputlocator translucency wobblywindows sheet glide; do
  echo "${effect}Enabled=false"
 done
} > "$defaults_dir/kwinrc"
cat > "$defaults_dir/breezerc" <<'CONF'
[Common]
ShadowSize=ShadowNone
CONF
cat > "$defaults_dir/ksplashrc" <<'CONF'
[KSplash]
Engine=none
Theme=None
CONF
cat > "$defaults_dir/baloofilerc" <<'CONF'
[Basic Settings]
Indexing-Enabled=false
CONF
cat > "$defaults_dir/kwalletrc" <<'CONF'
[Wallet]
Enabled=false
First Use=false
CONF
cat > "$defaults_dir/klaunchrc" <<'CONF'
[BusyCursorSettings]
Bouncing=false
[FeedbackStyle]
BusyCursor=false
CONF
cat > "$defaults_dir/ksmserverrc" <<'CONF'
[General]
loginMode=emptySession
CONF
{
 for module in browserintegrationreminder device_automounter freespacenotifier baloosearchmodule remotenotifier bluedevil kded_bolt printmanager donationmessage smart plasmavault kded_accounts; do
  printf '[Module-%s]\nautoload=false\n\n' "$module"
 done
} > "$defaults_dir/kded6rc"
chmod -R a+rX "$defaults_dir"
export XDG_CONFIG_DIRS="$defaults_dir:${XDG_CONFIG_DIRS:-/etc/xdg}"
export XDG_CURRENT_DESKTOP=KDE XDG_SESSION_DESKTOP=KDE XDG_SESSION_TYPE=wayland DESKTOP_SESSION=plasma KDE_FULL_SESSION=true KDE_SESSION_VERSION=6
export QT_QUICK_BACKEND=software QSG_RENDER_LOOP=basic QT_QPA_PLATFORMTHEME=kde QT_FORCE_STDERR_LOGGING=1
unset DISPLAY XAUTHORITY WAYLAND_DISPLAY QT_QPA_PLATFORM
LR_PLASMA_SOFTWARE=1
if (exec 3<>/dev/dri/renderD129) 2>/dev/null; then export KWIN_RENDER_NODES=/dev/dri/renderD129; LR_PLASMA_SOFTWARE=0
else export KWIN_RENDER_NODES=/dev/dri/renderD128; fi
export LIBGL_ALWAYS_SOFTWARE="$LR_PLASMA_SOFTWARE" LR_PLASMA_SOFTWARE
group=
cleanup() {
 trap - EXIT HUP INT TERM
 [ -z "$group" ] || [ "$(cat "$session/application-group" 2>/dev/null || true)" != "$group" ] || {
  kill -TERM "-$group" 2>/dev/null || true; sleep 1
  kill -KILL "-$group" 2>/dev/null || true
 }
 [ -z "$group" ] || wait "$group" 2>/dev/null || true
 envfile=$HOME/.cache/linuxrunner/desktop.env
 if grep -qx "XDG_RUNTIME_DIR=$runtime_dir" "$envfile" 2>/dev/null; then rm -f "$envfile"; fi
 rm -rf "$runtime_dir"
 rm -f "$session/password" "$session/cookie" "$session/authority"
 printf 'environment=plasma\ndisplay_server=wayland\nready=0\n' > "$session/desktop-state"
}
trap cleanup EXIT HUP INT TERM
stage session-start
setsid /bin/sh "$base/plasma-session.sh" "$session" group & group=$!
echo "PLASMA_SESSION: starting owned group=$group renderer=$KWIN_RENDER_NODES geometry=$geometry"
while kill -0 "$group" 2>/dev/null; do [ ! -e "$session/stop" ] || exit 0; sleep .5; done
status=0; wait "$group" || status=$?
exit "$status"
