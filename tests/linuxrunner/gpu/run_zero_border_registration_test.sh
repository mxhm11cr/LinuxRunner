#!/bin/sh
# Does not alter ANGLE or GPU libraries. Optional argument: patched virgl dylib.
set -eu
root=$(CDPATH= cd -- "$(dirname "$0")/../../.." && pwd)
cd "$root"
work=$(mktemp -d "${TMPDIR:-/tmp}/lr-zero-border.XXXXXXXX")
trap 'rm -rf "$work"' EXIT HUP INT TERM
for variant in none angle virgl both; do
    case "$variant" in
        none) flags= ;;
        angle) flags=-DTEST_ANGLE ;;
        virgl) flags=-DTEST_VIRGL ;;
        both) flags='-DTEST_ANGLE -DTEST_VIRGL' ;;
    esac
    # The flag set above is a fixed internal list, not user input.
    # shellcheck disable=SC2086
    xcrun clang -std=c11 -Wall -Wextra -Werror -Wl,-export_dynamic -I. $flags \
        tests/linuxrunner/gpu/zero_border_registration.c -o "$work/$variant"
    "$work/$variant"
    echo "ZERO_BORDER_REGISTRATION: $variant PASS"
done
if [ "$#" -gt 0 ]; then
    xcrun clang -std=c11 -Wall -Wextra -Werror -Wl,-export_dynamic -I. \
        tests/linuxrunner/gpu/zero_border_real_virgl.c -o "$work/real-virgl"
    "$work/real-virgl" "$1"
    echo 'ZERO_BORDER_REGISTRATION: real patched virgl PASS'
fi
