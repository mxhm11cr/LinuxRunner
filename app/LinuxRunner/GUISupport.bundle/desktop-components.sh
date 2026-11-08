#!/bin/sh
set -eu
cd "$(dirname "$0")"
session=$1
case "$session" in /*) ;; *) session="$PWD/$session";; esac
export LR_DESKTOP_SESSION="$session"
base="$PWD/desktop"
[ "$(id -u)" = 0 ] || base="${XDG_DATA_HOME:-$HOME/.local/share}/linuxrunner/desktop"
# Startup timeline (guest monotonic clock): DESKTOP_STARTUP: stage=… at_ms=… duration_ms=…
lr_ms() { lr_t=$(date +%s%N 2>/dev/null); case "$lr_t" in *[!0-9]*|'') awk '{printf "%d", $1 * 1000}' /proc/uptime 2>/dev/null || echo 0;; *) echo $((lr_t / 1000000));; esac; }
lr_start_ms=$(lr_ms); lr_stage_ms=$lr_start_ms
lr_stage() { lr_now=$(lr_ms); echo "DESKTOP_STARTUP: stage=$1 at_ms=$((lr_now - lr_start_ms)) duration_ms=$((lr_now - lr_stage_ms))${2:+ $2}"; lr_stage_ms=$lr_now; }
# Desktop environment: LinuxRunner (Openbox) by default; "plasma" (experimental,
# KDE Plasma X11 from the distribution's packages) when chosen in Settings ›
# Desktop (LR_DESKTOP_ENVIRONMENT from session.sh) or in this account's
# ~/.config/linuxrunner/desktop-environment. Plasma not installed: the regular
# desktop starts instead, so Open Desktop keeps working.
environment=${LR_DESKTOP_ENVIRONMENT:-}
[ -n "$environment" ] && [ "$environment" != linuxrunner ] || environment=$(cat "${XDG_CONFIG_HOME:-$HOME/.config}/linuxrunner/desktop-environment" 2>/dev/null || echo linuxrunner)
case "$environment" in plasma|lxqt|enlightenment|wayfire|hyprland|xfce) ;; *) environment=linuxrunner;; esac
if [ "$environment" = xfce ] && ! command -v startxfce4 >/dev/null; then echo 'DESKTOP_STARTUP: xfce=not-installed; starting the LinuxRunner desktop (Settings › Desktop Environment › Install Xfce)'; environment=linuxrunner; fi
if [ "$environment" = lxqt ] && ! command -v startlxqt >/dev/null; then echo 'DESKTOP_STARTUP: lxqt=not-installed; starting the LinuxRunner desktop (Settings › Desktop Environment › Install LXQt)'; environment=linuxrunner; fi
if [ "$environment" = enlightenment ] && ! command -v enlightenment_start >/dev/null; then echo 'DESKTOP_STARTUP: enlightenment=not-installed; starting the LinuxRunner desktop (Settings › Desktop Environment › Install Enlightenment)'; environment=linuxrunner; fi
if [ "$environment" = wayfire ] && ! command -v wayfire >/dev/null; then echo 'DESKTOP_STARTUP: wayfire=not-installed; starting the LinuxRunner desktop (Settings › Desktop Environment › Install Wayfire)'; environment=linuxrunner; fi
if [ "$environment" = hyprland ] && { ! command -v Hyprland >/dev/null || ! [ -c /dev/dri/card0 ]; }; then echo 'DESKTOP_STARTUP: hyprland=unavailable (not installed or no software KMS device); starting the LinuxRunner desktop'; environment=linuxrunner; fi
if [ "$environment" = enlightenment ] && [ "$(sed -n 's/^ID=//p' /etc/os-release 2>/dev/null | tr -d '"')" = debian ]; then echo 'DESKTOP_STARTUP: enlightenment=not-available-on-debian (stops at its splash screen); starting the LinuxRunner desktop'; environment=linuxrunner; fi
if [ "$environment" = plasma ] && ! command -v startplasma-x11 >/dev/null; then echo 'DESKTOP_STARTUP: plasma=not-installed; starting the LinuxRunner desktop (Settings › Desktop Environment › Install KDE Plasma)'; environment=linuxrunner; fi
[ -x ./desktop-probe ] || { echo 'DESKTOP_STARTUP: missing=desktop-probe; run Install Desktop Support'; exit 127; }
wm= panel= surface= conf= limit=120 integrated=0
qt_desktop=0
case "$(sed -n 's/^ID=//p' /etc/os-release 2>/dev/null | tr -d '\"')" in arch|archarm) qt_desktop=1;; esac
if [ "$environment" = linuxrunner ]; then
desktop_tools="openbox pcmanfm"
[ "$qt_desktop" = 0 ] || desktop_tools="openbox pcmanfm-qt lxqt-panel"
for tool in $desktop_tools; do command -v "$tool" >/dev/null || { echo "DESKTOP_STARTUP: missing=$tool; run Install Desktop Support"; exit 127; }; done
/bin/sh ./desktop-configure.sh
export XDG_CURRENT_DESKTOP=LXDE DESKTOP_SESSION=LinuxRunner
if [ "$qt_desktop" = 1 ]; then
 export XDG_CURRENT_DESKTOP=LXQt XDG_SESSION_TYPE=x11 QT_QPA_PLATFORM=xcb
 export QT_FORCE_STDERR_LOGGING=${QT_FORCE_STDERR_LOGGING:-1}
 echo 'DESKTOP_STARTUP: environment=linuxrunner components=openbox,lxqt-panel,pcmanfm-qt (Qt X11 desktop)'
fi
export XDG_DATA_DIRS="$base/share:${XDG_DATA_DIRS:-/usr/local/share:/usr/share}"
fi
# Programs started from LinuxRunner open inside a Wayland compositor: its
# socket joins the desktop environment file (see desktop-env.sh).
publish_wayland() {
 env_file="$HOME/.cache/linuxrunner/desktop.env"
 [ -f "$env_file" ] && grep -q '^DISPLAY=' "$env_file" || { echo "DESKTOP_STARTUP: wayland socket not published ($env_file missing)"; return 0; }
 { grep -v '^WAYLAND_DISPLAY=' "$env_file"; echo "WAYLAND_DISPLAY=$1"; } > "$env_file.new" && chmod 600 "$env_file.new" && mv "$env_file.new" "$env_file" &&
  echo "DESKTOP_STARTUP: wayland socket published for launched programs ($1)"
}
component_name() { [ "$environment" != plasma ] || { echo "Plasma session (plasma.log)"; return; }; [ "$environment" != lxqt ] || { echo "LXQt session (lxqt.log)"; return; }; [ "$environment" != xfce ] || { echo "Xfce session (xfce.log)"; return; }; [ "$environment" != enlightenment ] || { echo "Enlightenment (enlightenment.log)"; return; }; [ "$environment" != wayfire ] || { echo "Wayfire (wayfire.log)"; return; }; [ "$environment" != hyprland ] || { case "$1" in "$panel") echo "KMS display bridge (kms-bridge.log)";; *) echo "Hyprland (hyprland.log)";; esac; return; }; case "$1" in "$wm") echo "window manager (wm.log)";; "$panel") echo "panel (panel.log)";; "$surface") echo "desktop surface (desktop.log)";; *) echo unknown;; esac; }
cleanup() { trap - EXIT; for pid in "$surface" "$panel" "$wm" "$conf"; do [ -z "$pid" ] || kill "$pid" 2>/dev/null || true; done; }
trap cleanup EXIT
if [ "$environment" = plasma ]; then
 command -v startplasma-x11 >/dev/null || { echo 'DESKTOP_STARTUP: missing=startplasma-x11; install KDE Plasma (plasma-desktop kwin-x11)'; exit 127; }
 # One process tree (ksmserver, KWin, plasmashell): it is the window manager, panel and desktop.
 # No systemd: Plasma's session manager starts its components itself.
 export XDG_CURRENT_DESKTOP=KDE XDG_SESSION_TYPE=x11 KDE_FULL_SESSION=true KDE_SESSION_VERSION=6 DESKTOP_SESSION=plasma
 # iOS locks the device itself: Plasma's own idle screen locker would put a Linux
 # password prompt over the desktop after a few idle minutes. First start of an
 # account only; a kscreenlockerrc the user already has is left as it is.
 plasma_config="${XDG_CONFIG_HOME:-$HOME/.config}"
 if [ "$(sed -n 's/^ID=//p' /etc/os-release | tr -d '\"')" = steamos ]; then
  export KWIN_COMPOSE=N
  if [ "${LR_STEAMOS_GPU:-0}" = 1 ]; then
   unset QT_QUICK_BACKEND QT_OPENGL LIBGL_ALWAYS_SOFTWARE
   export QT_XCB_GL_INTEGRATION=xcb_egl QSG_RHI_BACKEND=opengl QSG_RHI_GL_USE_GLES=1
  else
   export QT_XCB_GL_INTEGRATION=none QT_QUICK_BACKEND=software QT_OPENGL=software
  fi
  mkdir -p "$plasma_config/autostart"
  /bin/sh ./steamos-fonts.sh || echo 'STEAMOS_FONTS: optional default unavailable; desktop startup continues'
  if [ ! -e "$plasma_config/startkderc" ] && [ ! -L "$plasma_config/startkderc" ]; then
   printf '[General]\nsystemdBoot=false\n' > "$plasma_config/startkderc"
  fi
  if [ ! -e "$plasma_config/autostart/steam.desktop" ] && [ ! -L "$plasma_config/autostart/steam.desktop" ]; then
   printf '[Desktop Entry]\nType=Application\nHidden=true\n' > "$plasma_config/autostart/steam.desktop"
  fi
  # The milestone is a desktop and terminal, without system services or updates.
  # Discover's Flatpak notifier crashes; Baloo reserves a 256 GiB index mapping;
  # the image's Python firewall/IBus applets also fail and consume address space.
  # XDG overrides apply to existing accounts too, preserving explicit choices.
  # iPadOS owns power, display brightness and authentication; their Linux
  # agents repeatedly query a system bus that this userspace does not provide.
  for applet in org.kde.discover.notifier baloo_file firewall-applet ibus powerdevil polkit-kde-authentication-agent-1; do
   if [ ! -e "$plasma_config/autostart/$applet.desktop" ] && [ ! -L "$plasma_config/autostart/$applet.desktop" ]; then
    printf '[Desktop Entry]\nType=Application\nHidden=true\n' > "$plasma_config/autostart/$applet.desktop"
   fi
  done
  # Tell KDE's Places model that indexing is disabled too. Merely suppressing
  # the daemon leaves it trying to enumerate the unavailable tags:/ location.
  if grep -qx 'Hidden=true' "$plasma_config/autostart/baloo_file.desktop" && command -v kwriteconfig6 >/dev/null; then
   kwriteconfig6 --file baloofilerc --group 'Basic Settings' --key Indexing-Enabled false
  fi
 fi
 if mkdir -p "$plasma_config" && [ ! -e "$plasma_config/kscreenlockerrc" ]; then
  printf '[Daemon]\nAutolock=false\nLockOnResume=false\n' > "$plasma_config/kscreenlockerrc" && echo 'DESKTOP_STARTUP: plasma screen locker defaults written (autolock off; iOS locks the device)'
 fi
 startplasma-x11 > "$session/plasma.log" 2>&1 & wm=$!; panel=$wm; surface=$wm; limit=960   # Plasma needs minutes under the interpreter
 echo "DESKTOP_STARTUP: environment=plasma (experimental) session=$wm"
elif [ "$environment" = lxqt ]; then
 # The full LXQt session (lxqt-session starts Openbox, lxqt-panel, the
 # pcmanfm-qt desktop and its own services); one process tree, like Plasma.
 export XDG_CURRENT_DESKTOP=LXQt XDG_SESSION_TYPE=x11 DESKTOP_SESSION=lxqt QT_QPA_PLATFORM=xcb
 export QT_QUICK_BACKEND=${QT_QUICK_BACKEND:-software} QT_FORCE_STDERR_LOGGING=${QT_FORCE_STDERR_LOGGING:-1}
 # First start of an account: a dark look (LXQt "system" theme drawn with a dark palette, Breeze Dark
 # icons, Onyx window borders, the LinuxRunner wallpaper) that reads well on a
 # TV. Every file the user already has, including LXQt's own, is left as it is.
 lxqt_config="${XDG_CONFIG_HOME:-$HOME/.config}"
 lxqt_default() { [ -e "$1" ] || [ -L "$1" ] || { mkdir -p "$(dirname "$1")" && cat > "$1.new" && mv "$1.new" "$1"; } || echo "DESKTOP_STARTUP: lxqt-default-skipped=$1"; }
 lxqt_default "$lxqt_config/lxqt/lxqt.conf" <<EOF || true
[General]
theme=system
icon_theme=breeze-dark
single_click_activate=false
tool_button_style=ToolButtonTextBesideIcon
palette_override=true

[Palette]
window_color=#1b2232
window_text_color=#e9f1ff
base_color=#141a27
text_color=#e9f1ff
highlight_color=#2f6fd6
highlighted_text_color=#ffffff
link_color=#7fb2ff
link_visited_color=#c0a6ff
tooltip_base_color=#1b2232
tooltip_text_color=#e9f1ff

[Qt]
font="Sans,11,-1,5,50,0,0,0,0,0"
doubleClickInterval=400
style=Fusion
wheelScrollLines=3
EOF
 lxqt_default "$lxqt_config/pcmanfm-qt/lxqt/settings.conf" <<EOF || true
[Desktop]
Wallpaper=$PWD/desktop-wallpaper.png
WallpaperMode=zoom
BgColor=#0b1425
FgColor=#e9f1ff
ShadowColor=#000000
EOF
 if [ -r /etc/xdg/openbox/rc.xml ] && [ -d /usr/share/themes/Onyx ]; then
  sed 's|<name>Clearlooks</name>|<name>Onyx</name>|' /etc/xdg/openbox/rc.xml | lxqt_default "$lxqt_config/openbox/rc.xml" || true
 fi
 startlxqt > "$session/lxqt.log" 2>&1 & wm=$!; panel=$wm; surface=$wm; limit=600
 echo "DESKTOP_STARTUP: environment=lxqt session=$wm"
elif [ "$environment" = xfce ]; then
 # The full Xfce session (xfce4-session starts xfwm4, xfce4-panel, xfdesktop
 # and the user's autostart). On Kali this is Kali's own Xfce desktop. The X
 # probe sees the window manager, panel dock and desktop window separately.
 export XDG_CURRENT_DESKTOP=XFCE XDG_SESSION_TYPE=x11 DESKTOP_SESSION=xfce
 # The distribution's default wallpaper is keyed to monitor0 (older xfdesktop
 # naming); xfdesktop 4.20 keys it by RandR output name ("screen" on this X
 # server), so a new account would get a black desktop. On an account's first
 # Xfce start, apply the distribution's own default image to the real output
 # name. An existing user desktop configuration is never touched.
 xfce_user_desktop="${XDG_CONFIG_HOME:-$HOME/.config}/xfce4/xfconf/xfce-perchannel-xml/xfce4-desktop.xml"
 if [ ! -e "$xfce_user_desktop" ] && [ ! -L "$xfce_user_desktop" ]; then
  xfce_image=$(sed -n 's|.*name="image-path" type="string" value="\([^"]*\)".*|\1|p' /etc/xdg/xfce4/xfconf/xfce-perchannel-xml/xfce4-desktop.xml 2>/dev/null | head -n 1)
  xfce_output=$(xrandr --listmonitors 2>/dev/null | sed -n 's/^ *0: [^ ]* .* \([^ ]*\)$/\1/p' | head -n 1)
  if [ -n "$xfce_image" ] && [ -r "$xfce_image" ] && [ -n "$xfce_output" ]; then
   mkdir -p "$(dirname "$xfce_user_desktop")" && cat > "$xfce_user_desktop.new" <<XFCEDESKTOP && mv "$xfce_user_desktop.new" "$xfce_user_desktop" && echo "DESKTOP_STARTUP: xfce_wallpaper=distribution-default output=$xfce_output"
<?xml version="1.0" encoding="UTF-8"?>
<channel name="xfce4-desktop" version="1.0">
  <property name="backdrop" type="empty">
    <property name="screen0" type="empty">
      <property name="monitor$xfce_output" type="empty">
        <property name="workspace0" type="empty">
          <property name="last-image" type="string" value="$xfce_image"/>
          <property name="image-style" type="int" value="5"/>
          <property name="color-style" type="int" value="0"/>
        </property>
      </property>
    </property>
  </property>
</channel>
XFCEDESKTOP
  fi
 fi
 # Responsiveness defaults: xfwm4's compositor and frame shadows double the
 # software drawing on this virtual X display (every window is drawn, then
 # composited, then captured). Applied as a system-level default for this
 # session only, built from the distribution's own xfwm4 settings (theme,
 # layout); the user's own Xfce settings still override it.
 xfce_defaults="$session/config-defaults/xfce4/xfconf/xfce-perchannel-xml"
 mkdir -p "$xfce_defaults"
 xfwm_system=
 for dir in $(printf '%s' "${XDG_CONFIG_DIRS:-/etc/xdg}" | tr ':' ' '); do
  [ -r "$dir/xfce4/xfconf/xfce-perchannel-xml/xfwm4.xml" ] && { xfwm_system="$dir/xfce4/xfconf/xfce-perchannel-xml/xfwm4.xml"; break; }
 done
 if [ -n "$xfwm_system" ] && grep -q '<property name="general" type="empty">' "$xfwm_system"; then
  sed -e '/name="use_compositing"/d' -e '/name="show_frame_shadow"/d' \
      -e 's|^\( *\)<property name="general" type="empty">|&\n\1  <property name="use_compositing" type="bool" value="false"/>\n\1  <property name="show_frame_shadow" type="bool" value="false"/>|' "$xfwm_system" > "$xfce_defaults/xfwm4.xml"
 else
  printf '%s\n' '<?xml version="1.0" encoding="UTF-8"?>' '<channel name="xfwm4" version="1.0">' '  <property name="general" type="empty">' '    <property name="use_compositing" type="bool" value="false"/>' '    <property name="show_frame_shadow" type="bool" value="false"/>' '  </property>' '</channel>' > "$xfce_defaults/xfwm4.xml"
 fi
 export XDG_CONFIG_DIRS="$session/config-defaults:${XDG_CONFIG_DIRS:-/etc/xdg}"
 echo 'DESKTOP_STARTUP: xfce_defaults=compositor-off,shadows-off (user settings override)'
 startxfce4 > "$session/xfce.log" 2>&1 & wm=$!; panel=$wm; surface=$wm; limit=600
 echo "DESKTOP_STARTUP: environment=xfce session=$wm"
elif [ "$environment" = enlightenment ]; then
 # Enlightenment is window manager, shelf and desktop in one process tree; it
 # composites in software. First start of an account: the "standard" profile
 # (shelf, pager, launcher bar) instead of "default", whose first-run wizard
 # nobody asked for; later starts keep whatever profile the user chose.
 export XDG_CURRENT_DESKTOP=Enlightenment XDG_SESSION_TYPE=x11 DESKTOP_SESSION=enlightenment
 # E's watchdog exits with 121 ("restart me") when the main loop has not
 # checked in within a time meant for native speed; under the interpreter its
 # normal startup work trips it and E restarted every ~10 s without ever
 # drawing. E_NO_WATCHDOG is E's own switch. The cost: a truly hung E is not
 # restarted automatically (the session's supervisor still sees it exit).
 export E_NO_WATCHDOG=1
 [ -e "$HOME/.e/e/config/profile.cfg" ] || export E_CONF_PROFILE=standard
 # Its shelf and wallpaper are drawn by its compositor, not separate dock and
 # desktop X windows: the X window manager selection is its readiness signal.
 integrated=1
 enlightenment_start > "$session/enlightenment.log" 2>&1 & wm=$!; panel=$wm; surface=$wm; limit=600
 echo "DESKTOP_STARTUP: environment=enlightenment session=$wm"
elif [ "$environment" = wayfire ]; then
 # Wayfire (wlroots) nested in this session's X server: wlroots' X11 backend
 # draws with its software renderer (pixman) into one full-screen window, so
 # the viewer, input and external-display path are those of every desktop.
 # GLES-only plugins (cube, blur, fire/spin) switch themselves off.
 export XDG_CURRENT_DESKTOP=Wayfire XDG_SESSION_TYPE=wayland DESKTOP_SESSION=wayfire
 export WLR_BACKENDS=x11 WLR_RENDERER=pixman WLR_X11_OUTPUTS=1
 # GTK 4 clients (wf-shell) draw in software: Mesa's EGL Wayland platform
 # currently crashes here (wl_proxy_create_wrapper), and the compositor has no
 # GPU to hand their buffers to anyway.
 export GSK_RENDERER=cairo
 wf_config="${XDG_CONFIG_HOME:-$HOME/.config}"
 if mkdir -p "$wf_config" && [ ! -e "$wf_config/wayfire.ini" ] && [ ! -L "$wf_config/wayfire.ini" ]; then
  # First start of an account: dark borders, short fades (cheap in software),
  # the panel at the bottom. Later starts keep the user's file.
  cat > "$wf_config/wayfire.ini.new" <<WAYFIRE_INI
[core]
plugins = alpha animate autostart command decoration expo fast-switcher foreign-toplevel grid gtk-shell move oswitch place resize scale vswitch wayfire-shell window-rules wm-actions xdg-activation zoom
background_color = \#0B1425FF
xwayland = true

[decoration]
active_color = \#1B2232FF
inactive_color = \#141A27FF
font = Sans 11
title_height = 28
border_size = 2
button_order = minimize maximize close

[animate]
open_animation = fade
close_animation = fade
duration = 150

[autostart]
autostart_wf_shell = true

[command]
binding_terminal = <super> KEY_ENTER
command_terminal = foot
WAYFIRE_INI
  mv "$wf_config/wayfire.ini.new" "$wf_config/wayfire.ini"
 fi
 # wf-panel's NetworkManager widget throws while connecting to a missing
 # system bus. Host socket networking does not provide NetworkManager. Include
 # the widget only when the real service owns its bus name; do not fake it.
 wf_network=none
 if [ -S /run/dbus/system_bus_socket ] && command -v dbus-send >/dev/null &&
  dbus-send --system --print-reply --dest=org.freedesktop.DBus /org/freedesktop/DBus org.freedesktop.DBus.NameHasOwner string:org.freedesktop.NetworkManager 2>/dev/null | grep -q 'boolean true'; then wf_network=network; fi
 sh ./wayfire-shell-configure.sh "$wf_config" "$wf_network"
 # GPU (Settings › Graphics, Metal): when the session prepared the virtio-gpu
 # card, wlroots drives it directly (DRM backend, GLES on virgl/Metal) and the
 # KMS bridge shows its output in this X server, as for Hyprland. Otherwise
 # Wayfire stays nested in the X server with the software renderer.
 output=X11-1
 if [ -c /dev/dri/card1 ] && [ -S /run/seatd.sock ] && command -v gcc >/dev/null; then
  export WLR_BACKENDS=drm WLR_DRM_DEVICES=/dev/dri/card1 WLR_RENDERER=gles2 LIBSEAT_BACKEND=seatd WLR_NO_HARDWARE_CURSORS=1
  output=Virtual-1; wf_gpu=1
  echo 'DESKTOP_STARTUP: wayfire renderer=virgl (GPU)'
 fi
 # The output always follows the session size (Settings › Display, the TV).
 geometry=$(xwininfo -root 2>/dev/null | awk '/-geometry/{split($2,g,"+"); print g[1]}')
 if [ -n "$geometry" ]; then
  awk '/^\[output:(X11-1|Virtual-1)\]/{skip=1;next} /^\[/{skip=0} !skip' "$wf_config/wayfire.ini" > "$wf_config/wayfire.ini.new" &&
   printf '\n[output:%s]\n# Managed by LinuxRunner: the session display size.\nmode = %s%s\n' "$output" "$geometry" "$([ "$output" = Virtual-1 ] && echo @60000)" >> "$wf_config/wayfire.ini.new" &&
   mv "$wf_config/wayfire.ini.new" "$wf_config/wayfire.ini"
 fi
 integrated=1
 if [ "${wf_gpu:-0}" = 1 ]; then
  short_runtime="/tmp/lrw-$(id -u)"
  if mkdir -p -m 700 "$short_runtime" 2>/dev/null && [ -O "$short_runtime" ] && [ ! -L "$short_runtime" ]; then
   chmod 700 "$short_runtime"; rm -rf "$short_runtime"/wayland-*; export XDG_RUNTIME_DIR="$short_runtime"; fi
  bridge_dir="${XDG_CACHE_HOME:-$HOME/.cache}/linuxrunner"
  bridge="$bridge_dir/lr-kms-bridge-$(sha256sum lr-kms-bridge.c | cut -c1-16)"
  if [ ! -x "$bridge" ]; then
   lr_stage desktop_config; echo 'DESKTOP_STARTUP: building the KMS display bridge (once per bridge version)'
   mkdir -p "$bridge_dir" && gcc -O2 lr-kms-bridge.c -lX11 -lXext -lwayland-client -lxkbcommon -o "$bridge.new" && mv "$bridge.new" "$bridge" ||
    { echo 'DESKTOP_STARTUP: wayfire bridge build failed'; exit 127; }
  fi
  wayfire > "$session/wayfire.log" 2>&1 & wm=$!; surface=$wm
  ( i=0
    until socket=$(ls "$XDG_RUNTIME_DIR" 2>/dev/null | grep -m1 '^wayland-[0-9]*$') && [ -S "$XDG_RUNTIME_DIR/$socket" ]; do
     i=$((i+1)); [ "$i" -lt 600 ] || exit 1; sleep .5; done
    publish_wayland "$XDG_RUNTIME_DIR/$socket"
    WAYLAND_DISPLAY=$socket exec "$bridge" /dev/dri/card1 ) >> "$session/kms-bridge.log" 2>&1 & panel=$!
 else
  wayfire > "$session/wayfire.log" 2>&1 & wm=$!; panel=$wm; surface=$wm
  ( i=0
    until socket=$(ls "$XDG_RUNTIME_DIR" 2>/dev/null | grep -m1 '^wayland-[0-9]*$') && [ -S "$XDG_RUNTIME_DIR/$socket" ]; do
     i=$((i+1)); [ "$i" -lt 600 ] || { echo "DESKTOP_STARTUP: no wayland socket in ${XDG_RUNTIME_DIR:-unset}"; exit 0; }; sleep .5; done
    publish_wayland "$XDG_RUNTIME_DIR/$socket" ) >> "$session/application.log" 2>&1 &
 fi
 limit=600
 echo "DESKTOP_STARTUP: environment=wayfire session=$wm output=${geometry:-unknown} renderer=$([ "${wf_gpu:-0}" = 1 ] && echo virgl || echo pixman)"
elif [ "$environment" = hyprland ]; then
 # Hyprland draws (GLES on llvmpipe, no GPU) to LinuxRunner's software KMS
 # device through seatd; lr-kms-bridge shows that output in this X server's
 # full-screen window and sends its input to Hyprland as a virtual pointer and
 # keyboard, so the viewer and external display are those of every desktop.
 export XDG_CURRENT_DESKTOP=Hyprland XDG_SESSION_TYPE=wayland DESKTOP_SESSION=hyprland LIBSEAT_BACKEND=seatd
 # foot and other clients need a UTF-8 locale; C.UTF-8 exists everywhere.
 case "${LANG:-}" in *UTF-8*|*utf8*) ;; *) export LANG=C.UTF-8;; esac
 export AQ_NO_ATOMIC=1 AQ_NO_MODIFIERS=1
 if [ -c /dev/dri/card1 ]; then
  # GPU acceleration: virtio-gpu (Mesa's virgl driver, executed on Metal).
  export AQ_DRM_DEVICES=/dev/dri/card1
  echo 'DESKTOP_STARTUP: hyprland renderer=virgl (GPU)'
 else
  # The device is CPU-rendered: Mesa's software KMS driver, not a GPU driver.
  export AQ_DRM_DEVICES=/dev/dri/card0 MESA_LOADER_DRIVER_OVERRIDE=kms_swrast
 fi
 # Hyprland's IPC sockets live in $XDG_RUNTIME_DIR/hypr/<66-character instance>/;
 # under the session directory they exceed the 108-byte socket path limit
 # (hyprctl and Waybar's Hyprland modules fail), so use a short private one.
 short_runtime="/tmp/lrh-$(id -u)"
 if mkdir -p -m 700 "$short_runtime" 2>/dev/null && [ -O "$short_runtime" ] && [ ! -L "$short_runtime" ]; then
  chmod 700 "$short_runtime"; rm -rf "$short_runtime/hypr" "$short_runtime"/wayland-*; export XDG_RUNTIME_DIR="$short_runtime"
 else echo "DESKTOP_STARTUP: hyprland runtime $short_runtime unavailable; Hyprland IPC may not work"; fi
 config="${XDG_CONFIG_HOME:-$HOME/.config}"
 # LinuxRunner preset 2 (hyprland-preset.lua, waybar-*, wallpaper). Written on
 # an account's first start; an earlier LinuxRunner preset is upgraded only if
 # it is still exactly as LinuxRunner wrote it (kept as *.linuxrunner-old).
 # Files the user changed are never replaced.
 preset_mark="$config/hypr/.linuxrunner-preset"
 mkdir -p "$config/hypr" "$config/waybar" "$HOME/.local/share/backgrounds"
 cp -f linuxrunner-wallpaper.png "$HOME/.local/share/backgrounds/linuxrunner-aurora.png" 2>/dev/null
 unmanaged_sum() { awk '/^-- LinuxRunner (display|effects) \(managed\) begin/{skip=1} !skip; /^-- LinuxRunner (display|effects) \(managed\) end/{skip=0}' "$1" | sha256sum | cut -c1-64; }
 install_preset() { # $1 source, $2 destination
  if [ -e "$2" ]; then cp -p "$2" "$2.linuxrunner-old"; fi
  cp "$1" "$2.new" && mv "$2.new" "$2"
 }
 lua="$config/hypr/hyprland.lua"
 if [ ! -e "$lua" ]; then upgrade=1
 elif [ -r "$preset_mark" ] && [ "$(unmanaged_sum "$lua")" = "$(sed -n 1p "$preset_mark")" ]; then upgrade=1   # an untouched LinuxRunner preset
 elif [ ! -e "$preset_mark" ] && grep -q '^        background_color        = "rgb(242933)",' "$lua" && grep -q 'hl.exec_cmd("waybar")' "$lua" &&
      [ "$(grep -c . "$lua")" -lt 330 ]; then upgrade=1   # preset 1 (generated from Hyprland's default)
 else upgrade=0; fi
 if [ "$upgrade" = 1 ] && ! cmp -s hyprland-preset.lua "$lua"; then
  install_preset hyprland-preset.lua "$lua"
  for f in config.jsonc style.css; do
   src=waybar-$f; [ "$f" = style.css ] && src=waybar-style.css; [ "$f" = config.jsonc ] && src=waybar-config.jsonc
   # Waybar preset 1 was short and generated here; keep any other file.
   if [ ! -e "$config/waybar/$f" ] || grep -q '#custom-launcher { background: #5e81ac' "$config/waybar/$f" 2>/dev/null ||
      grep -q '"format": "  Apps  "' "$config/waybar/$f" 2>/dev/null; then install_preset "$src" "$config/waybar/$f"; fi
  done
  echo "DESKTOP_STARTUP: hyprland preset=2 installed"
 fi
 # foot and fuzzel in the LinuxRunner palette (Nord), first start only.
 if mkdir -p "$config/foot" && [ ! -e "$config/foot/foot.ini" ]; then
  printf '[main]\nfont=JetBrains Mono:size=11,monospace:size=11\npad=12x10\n\n[colors-dark]\nalpha=0.96\nbackground=1e232d\nforeground=d8dee9\ncursor=1e232d 88c0d0\nregular0=3b4252\nregular1=bf616a\nregular2=a3be8c\nregular3=ebcb8b\nregular4=81a1c1\nregular5=b48ead\nregular6=88c0d0\nregular7=e5e9f0\nbright0=4c566a\nbright1=d08770\nbright2=b1d196\nbright3=f0d399\nbright4=8fbcbb\nbright5=c895bf\nbright6=93ccdc\nbright7=eceff4\n' > "$config/foot/foot.ini"
 fi
 # Programs from the launcher: software OpenGL while the desktop runs on the
 # GPU (see desktop-env.sh); LINUXRUNNER_GPU_APPS=1 keeps the GPU for them.
 mkdir -p "$HOME/.local/bin" && printf '#!/bin/sh\nif [ -c /dev/dri/card1 ] && [ "${LINUXRUNNER_GPU_APPS:-0}" != 1 ]; then export LIBGL_ALWAYS_SOFTWARE=1; fi\nexec "$@"\n' > "$HOME/.local/bin/linuxrunner-app" && chmod 755 "$HOME/.local/bin/linuxrunner-app"
 if mkdir -p "$config/fuzzel" && { [ ! -e "$config/fuzzel/fuzzel.ini" ] || grep -q '^anchor=bottom-left$' "$config/fuzzel/fuzzel.ini"; }; then
  printf '[main]\nlaunch-prefix=%s/.local/bin/linuxrunner-app\nanchor=top-left\ny-margin=52\nx-margin=10\nfont=Inter:size=12,Sans:size=12\nicon-theme=Papirus-Dark\nterminal=foot -e\nwidth=34\nlines=12\nhorizontal-pad=16\nvertical-pad=12\ninner-pad=6\n\n[colors]\nbackground=1e232df5\ntext=d8dee9ff\nmatch=88c0d0ff\nselection=3b4252ff\nselection-text=eceff4ff\nselection-match=88c0d0ff\nborder=88c0d0aa\n\n[border]\nwidth=1\nradius=12\n' "$HOME" > "$config/fuzzel/fuzzel.ini"
 fi
 if mkdir -p "$config/gtk-3.0" && [ ! -e "$config/gtk-3.0/settings.ini" ]; then
  printf '[Settings]\ngtk-application-prefer-dark-theme=1\ngtk-icon-theme-name=Papirus-Dark\ngtk-font-name=Inter 11\n' > "$config/gtk-3.0/settings.ini"
 fi
 # Managed blocks, rewritten at every start: the output follows the session
 # size (Settings › Display, the TV) and effects follow the renderer. Blur,
 # shadows and animations only with the GPU; on llvmpipe every frame is drawn
 # by the CPU, so they stay off and the desktop stays responsive.
 geometry=$(xwininfo -root 2>/dev/null | awk '/-geometry/{split($2,g,"+"); print g[1]}')
 effects_profile=$(cat "$session/hyprland-effects" 2>/dev/null || printf visual)
 case "$effects_profile" in performance|balanced|visual) ;; *) effects_profile=visual;; esac
 effects=$(sh ./hyprland-effects.sh "$effects_profile" "${AQ_DRM_DEVICES:-}")
 echo "DESKTOP_STARTUP: hyprland effects=$effects_profile device=${AQ_DRM_DEVICES:-software}"
 if [ -e "$lua" ]; then
  awk '/^-- LinuxRunner (display|effects) \(managed\) begin/{skip=1} !skip; /^-- LinuxRunner (display|effects) \(managed\) end/{skip=0}' "$lua" > "$lua.new" && {
   if [ -n "$geometry" ]; then printf -- '-- LinuxRunner display (managed) begin\nhl.monitor({ output = "Virtual-1", mode = "%s@60", position = "0x0", scale = 1 })\n-- LinuxRunner display (managed) end\n' "$geometry"; fi
   printf -- '-- LinuxRunner effects (managed) begin\n%s\n-- LinuxRunner effects (managed) end\n' "$effects"
  } >> "$lua.new" && mv "$lua.new" "$lua"
  # Remember the preset's own content so a later version can tell it was not edited.
  if [ "$upgrade" = 1 ]; then unmanaged_sum "$lua" > "$preset_mark"; fi
 fi
 command -v swaybg >/dev/null || echo 'DESKTOP_STARTUP: hyprland wallpaper needs swaybg (Repair Hyprland in Desktop Settings installs it); plain background'
 # The bridge is built once per account from this support version's source.
 bridge_dir="${XDG_CACHE_HOME:-$HOME/.cache}/linuxrunner"
 bridge="$bridge_dir/lr-kms-bridge-$(sha256sum lr-kms-bridge.c | cut -c1-16)"
 if [ ! -x "$bridge" ]; then
  lr_stage desktop_config; echo 'DESKTOP_STARTUP: building the KMS display bridge (once per bridge version)'
  mkdir -p "$bridge_dir" && gcc -O2 lr-kms-bridge.c -lX11 -lXext -lwayland-client -lxkbcommon -o "$bridge.new" && mv "$bridge.new" "$bridge" ||
   { echo 'DESKTOP_STARTUP: hyprland bridge build failed'; exit 127; }
 fi
 # start-hyprland (Hyprland 0.53 and later) is its supported launcher: it
 # starts Hyprland under a watchdog and restarts it in safe mode after a crash.
 if command -v start-hyprland >/dev/null; then start-hyprland > "$session/hyprland.log" 2>&1 & wm=$!
 else Hyprland > "$session/hyprland.log" 2>&1 & wm=$!; fi
 surface=$wm
 start_panel() {
  ( i=0
    until socket=$(ls "$XDG_RUNTIME_DIR" 2>/dev/null | grep -m1 '^wayland-[0-9]*$') && [ -S "$XDG_RUNTIME_DIR/$socket" ]; do
     i=$((i+1)); [ "$i" -lt 600 ] || exit 1; sleep .5; done
    # The bridge shows the scanout of the card Hyprland drives (virtio-gpu or software KMS).
    publish_wayland "$XDG_RUNTIME_DIR/$socket"
    WAYLAND_DISPLAY=$socket exec "$bridge" "${AQ_DRM_DEVICES%%:*}" ) >> "$session/kms-bridge.log" 2>&1 & panel=$!
 }
 start_panel
 integrated=1; limit=600
 echo "DESKTOP_STARTUP: environment=hyprland session=$wm bridge=$panel output=${geometry:-unknown}"
else
start_wm() { openbox --config-file "$base/config/openbox/rc.xml" >> "$session/wm.log" 2>&1 & wm=$!; }
if [ "$qt_desktop" = 1 ]; then
 start_panel() { lxqt-panel --config "$base/config/lxqt/panel.conf" >> "$session/panel.log" 2>&1 & panel=$!; }
elif command -v lxpanel >/dev/null; then
 # lxpanel reads its menu with the lxde- prefix (lxmenu-data). Where that menu
 # is absent but the distribution's own applications.menu exists (Fedora),
 # use the distribution's menu instead of showing an empty one.
 if [ -z "${XDG_MENU_PREFIX+x}" ]; then
  has_menu() { for d in $(printf '%s' "${XDG_CONFIG_DIRS:-/etc/xdg}" | tr ':' ' '); do [ -r "$d/menus/$1" ] && return 0; done; return 1; }
  if ! has_menu lxde-applications.menu && has_menu applications.menu; then
   export XDG_MENU_PREFIX=; echo 'DESKTOP_STARTUP: menu=distribution applications.menu'
  fi
 fi
 # lxpanel is GTK2, which ignores GTK3 settings. With no system or user
 # gtkrc (Fedora), GTK2 falls back to the empty hicolor icon theme and every
 # panel icon is blank; name the installed Adwaita theme then. A user's own
 # ~/.gtkrc-2.0 still applies after it.
 lr_gtk2rc=
 if [ ! -e /etc/gtk-2.0/gtkrc ] && [ ! -e "$HOME/.gtkrc-2.0" ] && [ -r /usr/share/icons/Adwaita/index.theme ]; then
  lr_gtk2rc="$base/config/gtkrc-2.0"
  [ -e "$lr_gtk2rc" ] || printf 'gtk-icon-theme-name = "Adwaita"\n' > "$lr_gtk2rc"
 fi
 start_panel() { GTK_THEME=Adwaita:dark GTK2_RC_FILES="${lr_gtk2rc:+$lr_gtk2rc:}$HOME/.gtkrc-2.0${GTK2_RC_FILES:+:$GTK2_RC_FILES}" lxpanel --profile LinuxRunner >> "$session/panel.log" 2>&1 & panel=$!; }
elif command -v xfce4-panel >/dev/null; then
 confbin=/usr/lib/xfce4/xfconf/xfconfd
 [ -x "$confbin" ] || confbin=/usr/libexec/xfce4/xfconf/xfconfd
 [ -x "$confbin" ] || { echo 'DESKTOP_STARTUP: xfconfd-missing'; exit 127; }
 "$confbin" > "$session/xfconf.log" 2>&1 & conf=$!
 start_panel() { GTK_THEME=Adwaita:dark xfce4-panel --disable-wm-check >> "$session/panel.log" 2>&1 & panel=$!; }
else echo 'DESKTOP_STARTUP: panel-missing'; exit 127; fi
start_surface() {
 if [ "$qt_desktop" = 1 ]; then pcmanfm-qt --profile LinuxRunner-X11 --desktop >> "$session/desktop.log" 2>&1 &
 else GTK_THEME=Adwaita:dark pcmanfm --profile LinuxRunner --desktop >> "$session/desktop.log" 2>&1 & fi
 surface=$!
}
start_wm; start_panel; start_surface
fi
# Supervision. A panel or desktop-surface crash must not end the session: the X
# server, window manager and open applications stay up and the component is
# restarted, at most $restart_limit times in a row (a run of $restart_window s
# or more resets the count). Every exit is reported with its status; restarts
# append to the component's log, so its first failure is kept. Past the limit
# the desktop continues without it (desktop-state: degraded=<roles>). The window
# manager is restarted the same way, but its final failure ends the session.
# Plasma is one process tree and is not restarted.
restart_limit=3 restart_window=120 failed=
wm_started=$(date +%s) panel_started=$(date +%s) surface_started=$(date +%s) wm_restarts=0 panel_restarts=0 surface_restarts=0
role_log() { case "$1" in wm) echo wm.log;; panel) echo panel.log;; *) echo desktop.log;; esac; }
role_name() { case "$1" in wm) echo "window manager (wm.log)";; panel) echo "panel (panel.log)";; *) echo "desktop surface (desktop.log)";; esac; }
write_degraded() {
 [ -f "$session/desktop-state" ] || return 0
 { grep -v -e '^degraded=' -e '^ready=' "$session/desktop-state" || true; echo "ready=0"; echo "degraded=$(echo $failed | tr ' ' ,)"; } > "$session/desktop-state.degraded"
 mv "$session/desktop-state.degraded" "$session/desktop-state"
}
supervise() {
 # Desktops that are one process tree end with it; Hyprland's display bridge
 # alone is restarted (a few times) while Hyprland runs.
 case "$environment" in plasma|lxqt|enlightenment|wayfire|hyprland)
  kill -0 "$wm" 2>/dev/null || { echo "$1: component_exited name=$(component_name "$wm") pid=$wm"; exit 1; }
  if [ "$environment" = hyprland ] && [ -n "$panel" ] && ! kill -0 "$panel" 2>/dev/null; then
   bridge_restarts=$((${bridge_restarts:-0} + 1))
   if [ "$bridge_restarts" -gt 5 ]; then echo "$1: component_failed name=$(component_name "$panel")"; panel=; failed="panel"; write_degraded
   else echo "$1: component_restarted name=$(component_name "$panel") attempt=$bridge_restarts/5"; sleep 1; start_panel; fi
  fi
  return 0;;
 esac
 for role in wm panel surface; do
  eval "pid=\$$role started=\$${role}_started restarts=\$${role}_restarts"
  [ -n "$pid" ] && ! kill -0 "$pid" 2>/dev/null || continue
  status=0; wait "$pid" 2>/dev/null || status=$?
  write_degraded
  ran=$(( $(date +%s) - started )); [ "$ran" -lt "$restart_window" ] || restarts=0
  echo "$1: component_exited name=$(role_name "$role") pid=$pid status=$status ran=${ran}s"
  if [ "$restarts" -ge "$restart_limit" ]; then
   [ "$role" != wm ] || { echo "$1: component_failed name=$(role_name wm) restarts=$restarts; ending the desktop session"; exit 1; }
   echo "$1: component_failed name=$(role_name "$role") restarts=$restarts; the desktop continues without it"
   eval "$role="; failed="${failed:+$failed }$role"; write_degraded; continue
  fi
  restarts=$((restarts + 1)); sleep "$restarts"
  printf '\n=== LinuxRunner: restart %s/%s after exit status %s ===\n' "$restarts" "$restart_limit" "$status" >> "$session/$(role_log "$role")"
  "start_$role"; eval "${role}_started=$(date +%s) ${role}_restarts=$restarts pid=\$$role"
  echo "$1: component_restarted name=$(role_name "$role") pid=$pid attempt=$restarts/$restart_limit"
 done
}
# Ready: a window manager, plus a panel and a desktop surface unless that one has
# permanently failed (then the state says degraded=...).
# Wayfire and Hyprland run no X window manager on this display: their mapped
# full-screen output window (Wayfire's own, Hyprland's KMS bridge) makes them ready.
nested_output_mapped() {
 root=$(xwininfo -root 2>/dev/null | awk '/-geometry/{split($2,g,"+"); print g[1]}')
 for w in $(xwininfo -root -children 2>/dev/null | awk -v g=" $root+0+0" 'index($0,g){print $1}'); do
  xwininfo -id "$w" 2>/dev/null | grep -q IsViewable && return 0
 done
 return 1
}
desktop_ready() {
 ./desktop-probe > "$session/desktop-state.probe" || return 1
 # KMS compositors (Hyprland; Wayfire on the GPU) are ready when the bridge has
 # presented a real frame from them, not when its window is mapped; Wayfire
 # nested in X when its own output window is mapped.
 case "$environment" in
  hyprland) ! grep -q 'KMS_BRIDGE: first frame' "$session/kms-bridge.log" 2>/dev/null || echo wm=1 >> "$session/desktop-state.probe";;
  wayfire) if [ "${wf_gpu:-0}" = 1 ]; then ! grep -q 'KMS_BRIDGE: first frame' "$session/kms-bridge.log" 2>/dev/null || echo wm=1 >> "$session/desktop-state.probe"
           else ! nested_output_mapped || echo wm=1 >> "$session/desktop-state.probe"; fi;;
 esac
 awk -F= -v failed=" $failed " -v integrated="$integrated" '{ v[$1] = $2 }
  END { complete = v["wm"] + 0 > 0 && (integrated || (v["panel"] + 0 > 0 && v["surface"] + 0 > 0)) && failed == "  "; ok = v["wm"] + 0 > 0 && (integrated || ((v["panel"] + 0 > 0 || index(failed, " panel ")) && (v["surface"] + 0 > 0 || index(failed, " surface "))))
        if (v["root_geometry"] != "") printf "root_geometry=%s\npanel_geometry=%s\nsurface_geometry=%s\n", v["root_geometry"], v["panel_geometry"], v["surface_geometry"];
        printf "wm=%s\npanel=%s\nsurface=%s\nready=%d\n", v["wm"], v["panel"], v["surface"], complete
        d = failed; gsub(/^ +| +$/, "", d); gsub(/ +/, ",", d); if (d != "") printf "degraded=%s\n", d
        exit !ok }' "$session/desktop-state.probe" > "$session/desktop-state.new"
}
i=0 started=$(date +%s)
lr_stage components_started "environment=$environment"
echo "DESKTOP_STARTUP: waiting for window manager, panel and desktop (wm=$wm panel=$panel surface=$surface)"
until desktop_ready; do
 supervise DESKTOP_STARTUP
 [ ! -e "$session/stop" ] || exit 0
 i=$((i+1)); [ "$i" -lt "$limit" ] || { echo "DESKTOP_STARTUP: readiness_timeout after $(( $(date +%s) - started ))s (inspect wm/panel/desktop logs)"; cat "$session/desktop-state.new"; head -n 120 /proc/ish/tasks; exit 1; }; sleep .5
done
lr_stage desktop_ready "environment=$environment (first frame for KMS compositors)"
echo "environment=$environment" >> "$session/desktop-state.new"
mv "$session/desktop-state.new" "$session/desktop-state"
echo "DESKTOP_STARTUP: ready=$(sed -n 's/^ready=//p' "$session/desktop-state") wm=PASS panel=$([ -n "$panel" ] && echo PASS || echo FAILED) surface=$([ -n "$surface" ] && echo PASS || echo FAILED) ready_after=$(( $(date +%s) - started ))s rendering=requires-frame-observation"
mkdir -p "$session/requests"
health_tick=0
while [ ! -e "$session/stop" ]; do
 supervise DESKTOP_SESSION
 health_tick=$((health_tick+1))
 if [ "$health_tick" -ge 10 ]; then
  health_tick=0
  if desktop_ready; then
   echo "environment=$environment" >> "$session/desktop-state.new"
   mv "$session/desktop-state.new" "$session/desktop-state"
  fi
 fi
 # launch-*: applications; input-*: the app's pointer round-trip readiness probe.
 for request in "$session"/requests/launch-*.sh "$session"/requests/input-*.sh; do
  [ -f "$request" ] && [ ! -L "$request" ] || continue
  name=${request##*/}; mv "$request" "$request.running"
  (code=0; /bin/sh "$request.running" >> "$session/application.log" 2>&1 || code=$?; printf '%s\n' "$code" > "$session/requests/$name.result"; rm -f "$request.running") &
  echo "DESKTOP_LAUNCH: request=$name started"
 done
 sleep .5
done
