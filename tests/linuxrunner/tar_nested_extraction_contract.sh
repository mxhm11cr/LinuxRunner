#!/bin/sh
# Run inside a disposable guest with its own tar, filesystem and permissions.
set -eu
work=$(mktemp -d "${TMPDIR:-/tmp}/lr-tar-contract.XXXXXXXX")
trap 'rm -rf "$work"' EXIT HUP INT TERM
mkdir -p "$work/source/a/b" "$work/destination"
printf 'LinuxRunner nested archive test\n' > "$work/source/a/b/test.txt"
chmod 751 "$work/source/a/b/test.txt"
ln -s b/test.txt "$work/source/a/link"
tar -czf "$work/archive.tar.gz" -C "$work/source" .
tar -xzf "$work/archive.tar.gz" -C "$work/destination"
cmp "$work/source/a/b/test.txt" "$work/destination/a/b/test.txt"
test "$(readlink "$work/destination/a/link")" = b/test.txt
test "$(stat -c %a "$work/destination/a/b/test.txt")" = 751
echo 'NESTED_TAR_EXTRACTION_PASS content permissions symlink'
