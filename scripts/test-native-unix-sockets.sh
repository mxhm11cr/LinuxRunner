#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
output=$(mktemp -d "${TMPDIR:-/tmp}/linuxrunner-unix-path-tests.XXXXXX")
trap 'rm -rf "$output"' EXIT
xcrun clang -fobjc-arc -Wall -Wextra -Werror -I. -framework Foundation \
    tests/linuxrunner/native_unix_socket_path_test.m -o "$output/unix-path-tests"
"$output/unix-path-tests"
