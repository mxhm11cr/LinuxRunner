#!/bin/sh
# Guest AF_UNIX sockets are backed by host sockets named <prefix><pid>.<id>.
# On iOS the prefix lives in the app container's temp directory (~82 bytes as
# /var/mobile/Containers/Data/Application/<UUID>/tmp/s). Build 900 used
# "/private/var/...tmp/ishsock<decimal pid>.<decimal id>", which exceeded
# sun_path (104) on devices with 5-digit PIDs: Xvfb "Cannot establish any
# listening sockets", D-Bus "File name too long" (GitHub report, iPad Air 4).
# Usage: af_unix_device_path_test.sh <ish> <debian-root-with-xvfb-and-dbus>
set -eu
ish=${1:?ish binary}; root=${2:?rootfs}
work=$(mktemp -d "${TMPDIR:-/tmp}/lr-sun-XXXXXX"); trap 'rm -rf "$work"' EXIT
pad() { d="$work/"; while [ ${#d} -lt "$1" ]; do d="${d}y"; done; mkdir -p "$d"; printf '%s/s' "$d"; }
probe='Xvfb :99 -screen 0 800x600x24 -nolisten tcp >/tmp/x99.log 2>&1 & p=$!; sleep 6
if kill -0 $p 2>/dev/null && DISPLAY=:99 xdpyinfo >/dev/null 2>&1; then echo XVFB_OK; else echo XVFB_FAIL; fi; kill $p 2>/dev/null
if dbus-daemon --session --address=unix:path=/tmp/lr-dbus-t --print-address --fork >/tmp/db.out 2>&1; then echo DBUS_OK; else cat /tmp/db.out; fi'
run() { ISH_SOCK_TMP_PREFIX="$1" "$ish" -f "$root" /bin/sh -c "$probe" 2>&1 | grep -E 'XVFB_|DBUS_|too long' || true; }
dev=$(pad 80); echo "device-length prefix: ${#dev} bytes"
out=$(run "$dev"); echo "$out"
echo "$out" | grep -q XVFB_OK && echo "$out" | grep -q DBUS_OK || { echo 'FAIL: device-length prefix'; exit 1; }
long=$(pad 98); echo "over-long prefix: ${#long} bytes (must fail honestly)"
out=$(run "$long"); echo "$out"
echo "$out" | grep -q 'File name too long' && echo "$out" | grep -q XVFB_FAIL || { echo 'FAIL: over-long prefix did not report ENAMETOOLONG'; exit 1; }
echo AF_UNIX_DEVICE_PATH_PASS
