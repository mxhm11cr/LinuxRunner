#!/bin/sh
set -eu
source_dir=$(CDPATH= cd -- "$(dirname "$0")/../../../app/LinuxRunner/GUISupport.bundle" && pwd)
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
cp "$source_dir/wayfire-shell-configure.sh" "$work/"
mkdir "$work/config"
sh "$work/wayfire-shell-configure.sh" "$work/config" none
grep -qx 'widgets_right = volume clock' "$work/config/wf-shell.ini"
grep -qx 'background_color = gtk_headerbar' "$work/config/wf-shell.ini"
echo 'PASS defaults do not require NetworkManager'
# Pinned legacy profile format, including its former invalid CSS color.
sed -e 's/widgets_right = volume clock/widgets_right = network volume clock/' -e 's/background_color = gtk_headerbar/background_color = rgba(11, 20, 37, 0.92)/' "$work/config/wf-shell.ini" > "$work/legacy"
cp "$work/legacy" "$work/config/wf-shell.ini"
sh "$work/wayfire-shell-configure.sh" "$work/config" none
grep -qx 'widgets_right = volume clock' "$work/config/wf-shell.ini"
cmp "$work/legacy" "$work"/config/wf-shell.ini.linuxrunner-backup.*
echo 'PASS exact legacy defaults migrated with intact backup'
printf '\n# custom preference\n' >> "$work/config/wf-shell.ini"
cp "$work/config/wf-shell.ini" "$work/custom"
sh "$work/wayfire-shell-configure.sh" "$work/config" network
cmp "$work/custom" "$work/config/wf-shell.ini"
echo 'PASS user configuration preserved'
mkdir "$work/linked"
ln -s "$work/custom" "$work/linked/wf-shell.ini"
sh "$work/wayfire-shell-configure.sh" "$work/linked" none
test -L "$work/linked/wf-shell.ini"
cmp "$work/custom" "$work/linked/wf-shell.ini"
echo 'PASS custom symlink preserved'
mkdir "$work/network"
sh "$work/wayfire-shell-configure.sh" "$work/network" network
grep -qx 'widgets_right = network volume clock' "$work/network/wf-shell.ini"
echo 'PASS actual NetworkManager option retained'
