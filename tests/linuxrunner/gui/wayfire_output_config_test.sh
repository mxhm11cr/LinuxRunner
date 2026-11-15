#!/bin/sh
set -eu
source_dir=$(CDPATH= cd -- "$(dirname "$0")/../../../app/LinuxRunner/GUISupport.bundle" && pwd)
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT HUP INT TERM
config=$work/wayfire.ini
cat > "$config" <<'CONFIG'
[core]
plugins = move
[output:X11-1]
mode = 640x480
scale = 1.5
position = 100,200
[output:Virtual-1]
mode = 1280x720@60000
transform = 90
CONFIG
sh "$source_dir/wayfire-output-configure.sh" "$config" X11-1 960x540
grep -qx 'mode = 960x540' "$config"
grep -qx 'scale = 1.5' "$config"
grep -qx 'position = 100,200' "$config"
grep -qx 'mode = 1280x720@60000' "$config"
grep -qx 'transform = 90' "$config"
sh "$source_dir/wayfire-output-configure.sh" "$config" Virtual-1 1920x1080
grep -qx 'mode = 1920x1080@60000' "$config"
grep -qx 'mode = 960x540' "$config"
grep -qx 'transform = 90' "$config"
cp "$config" "$work/expected"
ln -s "$config" "$work/link"
sh "$source_dir/wayfire-output-configure.sh" "$work/link" X11-1 640x480
[ -L "$work/link" ] && cmp "$config" "$work/expected"
if sh "$source_dir/wayfire-output-configure.sh" "$config" X11-1 'bad geometry'; then exit 1; fi
cmp "$config" "$work/expected"
printf '[core]\nplugins = move\n' > "$work/new.ini"
sh "$source_dir/wayfire-output-configure.sh" "$work/new.ini" X11-1 800x600
grep -qx '\[output:X11-1\]' "$work/new.ini"
grep -qx 'mode = 800x600' "$work/new.ini"
printf '%s\n' 'WAYFIRE_OUTPUT_CONFIG_PASS settings other-output symlink invalid-geometry'
