#!/bin/bash
# Run inside a disposable SteamFrame userspace, never against an installed user environment.
set -eu
. /etc/os-release
[ "$ID" = steamos ]
[ "$(uname -m)" = aarch64 ]
[ -n "$BASH_VERSION" ]
printf 'STEAMOS_BASH=%s\n' "$BASH_VERSION"
id
pacman --version
pacman -Q bash
work=$(mktemp -d /tmp/linuxrunner-steamos.XXXXXXXX)
trap 'rm -rf "$work"' EXIT
printf 'SteamOS userspace\n' > "$work/value"
[ "$(cat "$work/value")" = 'SteamOS userspace' ]
# Exercise fork/wait and signal return, including the guest vDSO path.
(/usr/bin/true) &
child=$!
wait "$child"
printf 'STEAMOS_TERMINAL=PASS\n'
