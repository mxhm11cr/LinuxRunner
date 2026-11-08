#!/bin/sh
# Publish new defaults or migrate only the exact old LinuxRunner template.
# Never rewrite a user-customized profile or follow an existing profile symlink.
set -eu
cd "$(dirname "$0")"
config=${1:?configuration directory required}
case "$config" in /*) ;; *) exit 2;; esac
case "${2:-none}" in network) widgets='network volume clock';; none) widgets='volume clock';; *) exit 2;; esac
file="$config/wf-shell.ini"
[ ! -L "$file" ] || { echo 'DESKTOP_CONFIG: wayfire-shell=custom-symlink preserved'; exit 0; }
mkdir -p "$config"
tmp=$(mktemp "$config/.linuxrunner-wf-shell.XXXXXX")
trap 'rm -f "$tmp"' EXIT
shell_defaults() {
 cat <<EOF
[panel]
position = bottom
autohide = false
minimal_height = 40
background_color = $2
widgets_left = menu spacing4 launchers window-list
widgets_center = none
widgets_right = $1
menu_icon_size = 32
launchers_size = 28
launcher_terminal = foot.desktop
clock_format = %a %e %b  %H:%M

[background]
image = $PWD/desktop-wallpaper.png
fill_mode = fill_and_crop

[dock]
autohide = true
EOF
}
if [ -e "$file" ]; then
 shell_defaults 'network volume clock' 'rgba(11, 20, 37, 0.92)' > "$tmp"
 if ! cmp -s "$tmp" "$file"; then
  echo 'DESKTOP_CONFIG: wayfire-shell=existing preserved'; exit 0
 fi
 backup=$(mktemp "$config/wf-shell.ini.linuxrunner-backup.XXXXXX")
 cp -p "$file" "$backup"
 echo 'DESKTOP_CONFIG: wayfire-shell=legacy-default backed-up'
fi
# wf-shell recognizes this theme value; its color parser rejects CSS rgba().
shell_defaults "$widgets" gtk_headerbar > "$tmp"
chmod 600 "$tmp"
mv "$tmp" "$file"
echo "DESKTOP_CONFIG: wayfire-shell=managed widgets-right=$widgets"
