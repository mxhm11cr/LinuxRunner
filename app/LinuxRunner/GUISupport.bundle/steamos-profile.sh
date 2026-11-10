#!/bin/sh
# Offline checks for the explicitly prepared SteamFrame desktop image.
set -eu
. "$(dirname "$0")/support-common.sh"
verify_resources
mode=${1:-check}
prepared=/usr/lib/linuxrunner/prebuilt-gui
required='Xvfb x11vnc xauth fc-match readelf dbus-daemon startplasma-x11 kwin_x11 plasmashell konsole setxkbmap xmodmap'
missing=
for tool in $required; do command -v "$tool" >/dev/null || missing="${missing}${missing:+,}$tool"; done
[ -z "$missing" ] || { echo "PROFILE_FAILURE: category=STEAMOS_IMAGE_INCOMPLETE stage=desktop detail=missing:$missing; prepare and import the matching image again"; exit 1; }
case "$mode" in
    repair|plasma)
        [ "$(id -u)" = 0 ] || support_fail PROFILE root-required
        echo 'PROFILE_STAGE: Preparing SteamOS display helpers'
        if [ -f "$prepared/binaries.sha256" ]; then
            # Prepared-archive installs carry helpers built when the archive was made.
            (cd "$prepared" && sha256sum -c binaries.sha256) || support_fail PROFILE steamos-helper-integrity
            # The prepared binaries must correspond to this build's helper sources.
            sha256sum -c "$prepared/sources.sha256" || support_fail PROFILE steamos-helper-source-mismatch
            for helper in gui-test focus-manager desktop-probe window-observer; do
                verify_elf PROFILE "$prepared/$helper"
                cp "$prepared/$helper" "$helper.new"; chmod 755 "$helper.new"; mv "$helper.new" "$helper"
            done
        else
            # Images installed from Valve's recovery image build them here with the image's own compiler.
            command -v gcc >/dev/null || support_fail PROFILE steamos-compiler-missing
            for helper in gui-test focus-manager desktop-probe window-observer; do
                gcc "$helper.c" -O2 -Wall -Wextra -lX11 -o "$helper.new" || support_fail PROFILE "steamos-helper-build:$helper"
                chmod 755 "$helper.new"; verify_elf PROFILE "$helper.new"
                mv "$helper.new" "$helper"
            done
        fi
        { cat support-version; sha256sum gui-test.c focus-manager.c; pkg-config --modversion x11 xcb; } | sha256sum | awk '{print $1}' > gui-build.key
        sha256sum desktop-probe.c | awk '{print $1}' > desktop-probe.key
        sha256sum window-observer.c | awk '{print $1}' > window-observer.key
        mkdir -p /var/lib/dbus
        dbus-uuidgen --ensure
        ;;
    check) ;;
    *) echo 'PROFILE_FAILURE: category=STEAMOS_EXPERIMENT stage=desktop detail=Only the prepared Plasma X11 profile is available'; exit 1;;
esac
missing=
for helper in gui-test focus-manager desktop-probe window-observer; do [ -x "$helper" ] || missing="${missing}${missing:+,}$helper"; done
if [ -z "$missing" ] && [ -f "$prepared/sources.sha256" ]; then
    sha256sum -c "$prepared/sources.sha256" >/dev/null || missing=helper-source-mismatch
fi
{
    if [ -n "$missing" ]; then echo "missing=$missing"; else echo ready; fi
    echo distribution=steamos
    echo plasma=installed
    echo optional-missing=audio
} > profile-check.result.tmp
mv profile-check.result.tmp profile-check.result
cat profile-check.result
[ -z "$missing" ] || [ "$mode" = check ]
