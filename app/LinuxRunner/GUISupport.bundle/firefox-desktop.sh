#!/bin/sh
# Keep package desktop entries intact. Put managed copies in our data directory.
set -eu
cd "$(dirname "$0")"
support=$PWD
applications=$1
mkdir -p "$applications"
# These files belong to LinuxRunner's desktop data directory, not user overrides.
for name in firefox.desktop firefox-esr.desktop org.mozilla.firefox.desktop; do
    source="/usr/share/applications/$name"
    destination="$applications/$name"
    [ ! -L "$destination" ] || { echo 'Firefox desktop destination is a symbolic link'; exit 1; }
    if [ ! -f "$source" ]; then
        if [ -f "$destination" ] && grep -qx '# LinuxRunner Firefox display policy' "$destination"; then rm -f "$destination"; fi
        continue
    fi
    try_exec=firefox
    [ "$name" != firefox-esr.desktop ] || try_exec=firefox-esr
    if grep -q '^TryExec=' "$source"; then try_exec=; fi
    temporary=$(mktemp "$applications/.firefox.XXXXXXXXXX")
    trap 'rm -f "$temporary"' EXIT HUP INT TERM
    awk -v launcher="$support/firefox-launch.sh" -v try_exec="$try_exec" '
        BEGIN { print "# LinuxRunner Firefox display policy" }
        /^\[/ { entry=($0=="[Desktop Entry]") }
        /^\[Desktop Entry\]$/ { print; print "DBusActivatable=false"; if(try_exec!="")print "TryExec=" try_exec; next }
        entry && /^DBusActivatable=/ { next }
        /^Exec=/ { print "Exec=/bin/sh \"" launcher "\" " substr($0,6); next }
        { print }
    ' "$source" > "$temporary"
    chmod 644 "$temporary"
    mv "$temporary" "$destination"
    trap - EXIT HUP INT TERM
done
