#!/bin/sh
# Native policy/request contract; does not grant or test iOS JIT permission.
set -eu
source_root=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
output=$(mktemp -d "${TMPDIR:-/tmp}/lr-jit-activation.XXXXXX")
trap 'rm -rf "$output"' EXIT HUP INT TERM
cd "$source_root"
cc -std=c11 -Wall -Wextra -Werror -I. tests/linuxrunner/jit_activation_policy.c -o "$output/policy"
"$output/policy"
echo 'JIT_ACTIVATION_POLICY PASS (16 combinations)'
xcrun clang -fobjc-arc -Wall -Wextra -Werror -I. tests/linuxrunner/stikdebug_request.m -framework Foundation -o "$output/request"
"$output/request"
echo 'STIKDEBUG_REQUEST PASS (target, PID, universal.js, invalid arguments)'
