#!/bin/sh
# Shared, distro-local checks. Metadata is parsed as data, never evaluated.
SUPPORT_DIR=$(CDPATH= cd -- "$(dirname "$0")" && pwd)
cd "$SUPPORT_DIR"
SUPPORT_VERSION=$(cat support-version 2>/dev/null || true)
# Unattended dpkg configuration-file answers for every apt run of these
# scripts (apt-unattended.conf); the user's own terminal apt is unaffected.
if [ -r "$SUPPORT_DIR/apt-unattended.conf" ] && [ -z "${APT_CONFIG:-}" ]; then export APT_CONFIG="$SUPPORT_DIR/apt-unattended.conf"; fi
support_fail() { echo "$1: result=FAIL reason=$2"; exit "${3:-1}"; }
verify_resources() {
    [ -n "$SUPPORT_VERSION" ] || support_fail SUPPORT_DEPLOY version-missing
    echo "SUPPORT_DEPLOY: version=$SUPPORT_VERSION destination=$SUPPORT_DIR"
    # Managed support files can be root-owned/read-only when an ordinary user
    # launches an application. Verification must not require writing there.
    if ! resource_check=$(sha256sum -c support.sha256 2>&1); then
        printf '%s\n' "$resource_check"
        support_fail SUPPORT_DEPLOY checksum-or-resource-missing
    fi
    # Native deployment sets host modes; fakefs also keeps guest mode metadata.
    # Publish executable permissions there for each verified managed script.
    while read -r checksum resource; do
        case "$resource" in *.sh) [ -x "$resource" ] || chmod 755 "$resource" || support_fail SUPPORT_DEPLOY executable-permission-failed;; esac
    done < support.sha256
    echo 'SUPPORT_DEPLOY: source/scripts/checksums/modes=verified result=PASS'
}
metadata_value() { awk -F= -v key="$2" '$1==key {print substr($0,length(key)+2); exit}' "$1" 2>/dev/null; }
verify_shm_namespace() (
    prefix=$1
    [ -d /dev/shm ] || support_fail "$prefix" shared-memory-directory-missing
    mode=$(stat -c %a /dev/shm) || support_fail "$prefix" shared-memory-stat-failed
    [ -w /dev/shm ] && [ -x /dev/shm ] || support_fail "$prefix" shared-memory-directory-inaccessible
    probe=$(mktemp /dev/shm/.linuxrunner-check.XXXXXX) || support_fail "$prefix" shared-memory-file-create-failed
    rm "$probe" || support_fail "$prefix" shared-memory-file-unlink-failed
    echo "$prefix: shared_memory=/dev/shm mode=$mode create/unlink=PASS guest-page=$(getconf PAGESIZE)"
)
verify_elf() (
    prefix=$1; binary=$2
    [ -f "$binary" ] || support_fail "$prefix" "binary-missing:$binary" 127
    [ -x "$binary" ] || support_fail "$prefix" "executable-permission-missing:$binary" 126
    command -v readelf >/dev/null || support_fail "$prefix" readelf-missing
    header=$(readelf -h "$binary" 2>&1) || support_fail "$prefix" "invalid-linux-elf:$binary" 126
    printf '%s
' "$header" | grep -q 'Machine:.*AArch64' || support_fail "$prefix" "invalid-architecture:$binary" 126
    printf '%s
' "$header" | grep -q 'Class:.*ELF64' || support_fail "$prefix" "invalid-elf-class:$binary" 126
    loader=$(readelf -l "$binary" | sed -n 's/.*interpreter: \(.*\)\]/\1/p')
    [ -z "$loader" ] || [ -x "$loader" ] || support_fail "$prefix" "dynamic-loader-missing:$loader" 126
    dependencies=$(ldd "$binary" 2>&1) || { printf '%s
' "$dependencies"; support_fail "$prefix" dynamic-dependency-failed 126; }
    if printf '%s
' "$dependencies" | grep -Eq 'not found|Error loading|Error relocating'; then
        printf '%s
' "$dependencies"; support_fail "$prefix" dynamic-library-missing 126
    fi
    echo "$prefix: executable_path=$binary exists=yes executable=yes architecture=aarch64 dependencies=resolved"
)
# Headers are installation dependencies, not launch dependencies. Package
# versions plus source/format version determine when a new build is required.
browser_packages() {
    if command -v apk >/dev/null; then apk info -v qt6-qtbase qt6-qtwebengine
    elif command -v dpkg-query >/dev/null; then
        dpkg-query -W -f='${Package}=${Version}\n' libqt6widgets6 libqt6webenginewidgets6 libqt6webenginecore6-bin
    elif command -v pacman >/dev/null; then pacman -Q qt6-base qt6-webengine
    else return 1; fi
}
browser_key() {
    { cat support-version; sha256sum browser.cpp; browser_packages; } | sha256sum | awk '{print $1}'
}
browser_ready() (
    [ -f browser-support.meta ] && [ -x browser ] || exit 1
    [ "$(metadata_value browser-support.meta version)" = "$SUPPORT_VERSION" ] || exit 1
    [ "$(metadata_value browser-support.meta fingerprint)" = "$(browser_key)" ] || exit 1
    [ "$(metadata_value browser-support.meta binary_sha256)" = "$(sha256sum browser | awk '{print $1}')" ] || exit 1
    verify_elf BROWSER_CHECK "$SUPPORT_DIR/browser" >/dev/null && ./browser --versions >/dev/null 2>&1
)
