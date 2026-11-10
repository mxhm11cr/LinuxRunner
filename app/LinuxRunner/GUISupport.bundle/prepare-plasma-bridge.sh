#!/bin/sh
# Build against this rootfs's real Qt/KDE libraries. No package/security changes.
set -eu
cd "$(dirname "$0")"
output=${1:-$PWD/lr-plasma-bridge}
case "$output" in /*) ;; *) echo 'PLASMA_BUILD: absolute output path required'; exit 2;; esac
[ ! -L "$output" ] && [ ! -L "$output.key" ] || { echo 'PLASMA_BUILD: symbolic-link output refused'; exit 2; }
pkg-config --exists 'libei-1.0 >= 1.6' Qt6Widgets Qt6DBus KF6CoreAddons libvncserver || {
 echo 'PLASMA_BUILD: missing Qt/KDE/libei development dependencies'; exit 3;
}
libdir=$(pkg-config --variable=libdir libvncserver)
[ -f "$libdir/libkrfbprivate.so.5" ] || { echo 'PLASMA_BUILD: KRFB capture library missing'; exit 3; }
key=$({ sha256sum lr-plasma-bridge.cpp lr-plasma-krfb.h prepare-plasma-bridge.sh "$libdir/libkrfbprivate.so.5"; pkg-config --modversion Qt6Widgets Qt6DBus KF6CoreAddons libei-1.0 libvncserver; } | sha256sum | awk '{print $1}')
if [ -x "$output" ] && [ "$(cat "$output.key" 2>/dev/null || true)" = "$key" ]; then echo 'PLASMA_BUILD: existing helper matches source and libraries'; exit 0; fi
echo 'PLASMA_BUILD: building helper (one time after an update)'
mkdir -p "$(dirname "$output")"
temporary="$output.new.$$"
trap 'rm -f "$temporary" "$temporary.key"' EXIT HUP INT TERM
[ ! -e "$temporary" ] || exit 2
${CXX:-c++} -std=c++17 -O2 -fPIC $(pkg-config --cflags Qt6Widgets Qt6DBus KF6CoreAddons libei-1.0 libvncserver) \
 lr-plasma-bridge.cpp $(pkg-config --libs Qt6Widgets Qt6DBus KF6CoreAddons libei-1.0 libvncserver) \
 -Wl,-l:libkrfbprivate.so.5 -o "$temporary"
readelf -h "$temporary" | grep -q 'Machine:.*AArch64' || { echo 'PLASMA_BUILD: expected an AArch64 ELF'; exit 4; }
chmod 755 "$temporary"
printf '%s\n' "$key" > "$temporary.key"
mv "$temporary" "$output"
mv "$temporary.key" "$output.key"
echo 'PLASMA_BUILD: compiled; desktop runtime verification is separate'
