#!/bin/sh
# Install the app-pinned, isolated driver into an existing SteamOS root.
# Called as root before the desktop changes to its selected Linux account.
set -eu
. "$(dirname "$0")/support-common.sh"
[ "$(id -u)" = 0 ] || support_fail GPU_OVERLAY root-required
[ "$(sed -n 's/^ID=//p' /etc/os-release | tr -d '\"')" = steamos ] || exit 1
[ -f ./steamos-gpu.tar.gz ] && [ -f ./steamos-gpu.sha256 ] || { echo 'GPU_OVERLAY: unavailable; using software'; exit 1; }
verify_resources
sha256sum -c steamos-gpu.sha256 || support_fail GPU_OVERLAY archive-checksum
expected=$(awk 'NR==1 {print $1}' steamos-gpu.sha256)
case "$expected" in ''|*[!0-9a-f]*) support_fail GPU_OVERLAY invalid-digest;; esac
[ "${#expected}" = 64 ] || support_fail GPU_OVERLAY invalid-digest
prefix=/opt/linuxrunner/steamos-gpu
for directory in /opt /opt/linuxrunner "$prefix"; do
    [ ! -L "$directory" ] || support_fail GPU_OVERLAY symbolic-link-prefix
    [ ! -e "$directory" ] || { [ -d "$directory" ] && [ "$(stat -c %u "$directory")" = 0 ]; } || support_fail GPU_OVERLAY unmanaged-prefix
done
if [ "$(cat "$prefix/archive.sha256" 2>/dev/null || true)" = "$expected" ] &&
    (cd "$prefix" && sha256sum -c payload.sha256 >/dev/null 2>&1); then
    echo 'GPU_OVERLAY: ready (isolated Mesa virgl)'; exit 0
fi
umask 022
mkdir -p /opt/linuxrunner
stage=$(mktemp -d /opt/linuxrunner/.steamos-gpu.XXXXXXXX)
cleanup() {
    if [ -d "$stage/previous" ] && [ ! -e "$prefix" ]; then mv "$stage/previous" "$prefix" || true; fi
    rm -rf "$stage"
}
trap cleanup EXIT
trap 'exit 1' HUP INT TERM
tar -xzf ./steamos-gpu.tar.gz -C "$stage"
[ -d "$stage/steamos-gpu" ] && [ ! -L "$stage/steamos-gpu" ] || support_fail GPU_OVERLAY invalid-payload
(cd "$stage/steamos-gpu" && sha256sum -c payload.sha256) || support_fail GPU_OVERLAY payload-checksum
printf '%s\n' "$expected" > "$stage/steamos-gpu/archive.sha256"
[ ! -e "$prefix" ] || mv "$prefix" "$stage/previous"
mv "$stage/steamos-gpu" "$prefix"
echo 'GPU_OVERLAY: installed (system Mesa preserved)'
