#!/bin/sh
set -eu
cd "$(dirname "$0")"
base="$PWD/desktop"
[ "$(id -u)" = 0 ] || base="${XDG_DATA_HOME:-$HOME/.local/share}/linuxrunner/desktop"
mkdir -p "$base/config/openbox" "$base/config/lxpanel/LinuxRunner/panels" "$base/config/pcmanfm/LinuxRunner" "$base/share/themes/LinuxRunner/openbox-3" "$base/config/gtk-3.0" "$base/config/xfce4/xfconf/xfce-perchannel-xml"
# Dedicated component configuration: never replace guest home, Telegram/browser
# profiles or existing ~/.config. Publish defaults only when absent.
put() { [ -e "$2" ] || { cp "$1" "$2.new"; chmod 600 "$2.new"; mv "$2.new" "$2"; }; }
put desktop-openbox.xml "$base/config/openbox/rc.xml"
put desktop-panel.conf "$base/config/lxpanel/LinuxRunner/panels/panel"
# Some distributions ship xterm.desktop rather than Debian's launcher ID.
# Repair only an exact managed default; keep every customized panel intact.
panel="$base/config/lxpanel/LinuxRunner/panels/panel"
if [ ! -e /usr/share/applications/debian-xterm.desktop ] &&
   [ -r /usr/share/applications/xterm.desktop ] &&
   [ "$(sha256sum < "$panel")" = "$(sha256sum < desktop-panel.conf)" ]; then
 sed 's|id=/usr/share/applications/debian-xterm.desktop|id=/usr/share/applications/xterm.desktop|' "$panel" > "$panel.new"
 chmod 600 "$panel.new"; mv "$panel.new" "$panel"
 echo 'DESKTOP_CONFIG: managed panel terminal launcher resolved from installed desktop entry'
fi
put desktop-theme.conf "$base/share/themes/LinuxRunner/openbox-3/themerc"
if [ ! -e "$base/config/pcmanfm/LinuxRunner/desktop-items-0.conf" ]; then
 cat > "$base/config/pcmanfm/LinuxRunner/desktop-items-0.conf.new" <<EOF
[*]
wallpaper_mode=fit
wallpaper_common=1
wallpaper=$PWD/desktop-wallpaper.png
desktop_bg=#0b1425
desktop_fg=#e9f1ff
desktop_shadow=#000000
desktop_font=DejaVu Sans 11
show_wm_menu=1
show_documents=0
show_trash=1
show_mounts=0
EOF
 mv "$base/config/pcmanfm/LinuxRunner/desktop-items-0.conf.new" "$base/config/pcmanfm/LinuxRunner/desktop-items-0.conf"
fi
if [ ! -e "$base/config/gtk-3.0/settings.ini" ]; then
 cat > "$base/config/gtk-3.0/settings.ini.new" <<EOF
[Settings]
gtk-theme-name=Adwaita
gtk-application-prefer-dark-theme=1
gtk-icon-theme-name=Adwaita
gtk-font-name=DejaVu Sans 11
EOF
 mv "$base/config/gtk-3.0/settings.ini.new" "$base/config/gtk-3.0/settings.ini"
fi
put desktop-xfce-panel.xml "$base/config/xfce4/xfconf/xfce-perchannel-xml/xfce4-panel.xml"
put desktop-pcmanfm.conf "$base/config/pcmanfm/LinuxRunner/pcmanfm.conf"
# Expose only named component profiles through the user's real XDG directory.
# Changing a panel process's XDG_CONFIG_HOME also changes every app it launches.
# Keep the original environment, and link owned profiles without overwriting any
# existing user path (including a deliberately customized/dangling symlink).
user_config=${XDG_CONFIG_HOME:-${HOME:?HOME is required}/.config}
case "$user_config" in /*) ;; *) echo 'DESKTOP_CONFIG: XDG_CONFIG_HOME must be absolute'; exit 2;; esac
profile_link() {
 destination=$2
 if [ ! -e "$destination" ] && [ ! -L "$destination" ]; then
  mkdir -p "$(dirname "$destination")"
  ln -s "$1" "$destination"
 fi
}
profile_link "$base/config/lxpanel/LinuxRunner" "$user_config/lxpanel/LinuxRunner"
profile_link "$base/config/pcmanfm/LinuxRunner" "$user_config/pcmanfm/LinuxRunner"
# Only Alpine's LinuxRunner desktop uses xfce4-panel as its panel. Elsewhere a
# user-level xfce4-panel.xml would override a real Xfce desktop's own panel
# (for example Kali's), so it is not linked there.
case "$(sed -n 's/^ID=//p' /etc/os-release 2>/dev/null | tr -d '"')" in
 alpine) profile_link "$base/config/xfce4/xfconf/xfce-perchannel-xml/xfce4-panel.xml" "$user_config/xfce4/xfconf/xfce-perchannel-xml/xfce4-panel.xml";;
esac
printf 'schema=1\nconfiguration=first-write-only\n' > "$base/configuration.meta.new"
mv "$base/configuration.meta.new" "$base/configuration.meta"
echo "DESKTOP_CONFIG: version=1 destination=$base user_files=preserved result=PASS"
# Arch's Qt applications use a different schema from LXPanel/GTK PCManFM.
# A new named app profile leaves the old auto-generated (or customized) Qt
# profile intact. Never replace the user's normal LXQt panel configuration.
desktop_distro=$(sed -n 's/^ID=//p' /etc/os-release 2>/dev/null | tr -d '\"')
# Explicit component defaults can also be generated without an Arch host.
[ "${1:-}" != qt-x11 ] || desktop_distro=arch
case "$desktop_distro" in arch|archarm)
 mkdir -p "$base/config/lxqt" "$base/config/pcmanfm-qt/LinuxRunner-X11" "$base/share/applications"
 put desktop-qt-panel.conf "$base/config/lxqt/panel.conf"
 if [ ! -e "$base/config/pcmanfm-qt/LinuxRunner-X11/settings.conf" ]; then
  cat > "$base/config/pcmanfm-qt/LinuxRunner-X11/settings.conf.new" <<EOF
[System]
FallbackIconThemeName=Adwaita
Terminal=xterm
[Desktop]
WallpaperMode=fit
Wallpaper=$PWD/desktop-wallpaper.png
BgColor=#0b1425
FgColor=#e9f1ff
ShadowColor=#000000
HideItems=false
DesktopIconSize=48
[Volume]
MountOnStartup=false
MountRemovable=false
AutoRun=false
[Window]
LastWindowWidth=680
LastWindowHeight=420
EOF
  mv "$base/config/pcmanfm-qt/LinuxRunner-X11/settings.conf.new" "$base/config/pcmanfm-qt/LinuxRunner-X11/settings.conf"
 fi
 profile_link "$base/config/pcmanfm-qt/LinuxRunner-X11" "$user_config/pcmanfm-qt/LinuxRunner-X11"
 for app in Terminal Files; do
  case "$app" in Terminal) executable=xterm; icon=utilities-terminal;; Files) executable='pcmanfm-qt --profile LinuxRunner-X11'; icon=system-file-manager;; esac
  target="$base/share/applications/linuxrunner-$app.desktop"
  if [ ! -e "$target" ]; then
   printf '[Desktop Entry]\nType=Application\nName=%s\nExec=%s\nIcon=%s\nTerminal=false\nCategories=Utility;\n' "$app" "$executable" "$icon" > "$target.new"
   chmod 644 "$target.new"; mv "$target.new" "$target"
  fi
  desktop_dir="$HOME/Desktop"
  if command -v xdg-user-dir >/dev/null; then desktop_dir=$(xdg-user-dir DESKTOP); fi
  case "$desktop_dir" in /*) mkdir -p "$desktop_dir"; if [ ! -e "$desktop_dir/LinuxRunner-$app.desktop" ] && [ ! -L "$desktop_dir/LinuxRunner-$app.desktop" ]; then put "$target" "$desktop_dir/LinuxRunner-$app.desktop"; chmod u+x "$desktop_dir/LinuxRunner-$app.desktop"; fi;; esac
 done
 echo 'DESKTOP_CONFIG: qt-profile=LinuxRunner-X11 panel=managed configuration=first-write-only old-profiles=preserved'
;; esac
