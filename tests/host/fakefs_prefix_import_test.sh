#!/bin/sh
# fakefs_import_prefix contract (Kali's rootfs lives under kali-arm64/):
# the top-level directory becomes /, hardlinks are rewritten, and any entry or
# hardlink outside the expected directory makes the whole import fail.
# fakefs keeps a symlink as a host file holding its target (type in meta.db).
# Usage: fakefs_prefix_import_test.sh <fakefsify>
set -eu
tool=${1:?fakefsify path}
work=$(mktemp -d "${TMPDIR:-/tmp}/lr-prefix-XXXXXX")
trap 'rm -rf "$work"' EXIT
cd "$work"
mkdir -p good/top/etc good/top/usr/bin outside
printf 'ok\n' > good/top/etc/x
ln good/top/etc/x good/top/etc/hardlink
ln -s ../../etc/x good/top/usr/bin/link
printf 'escape\n' > outside/y
bsdtar -cf good.tar -C good top
bsdtar -cf bad.tar -C good top -C "$work" outside
bsdtar -cf - -C "$work" outside | bsdtar -cf mixed.tar @good.tar @-
"$tool" good.tar root top
[ -f root/data/etc/x ] && [ -f root/data/etc/hardlink ] && [ -e root/data/usr/bin/link ] && [ "$(cat root/data/usr/bin/link)" = ../../etc/x ] || { echo 'FAIL: stripped layout'; exit 1; }
[ "$(stat -f %i root/data/etc/x)" = "$(stat -f %i root/data/etc/hardlink)" ] || { echo 'FAIL: hardlink not preserved'; exit 1; }
[ ! -e root/data/top ] || { echo 'FAIL: prefix directory kept'; exit 1; }
if "$tool" bad.tar refused top 2>/dev/null; then echo 'FAIL: entry outside prefix accepted'; exit 1; fi
if "$tool" mixed.tar refused2 top 2>/dev/null; then echo 'FAIL: appended outside entry accepted'; exit 1; fi
if "$tool" good.tar wrongprefix other 2>/dev/null; then echo 'FAIL: wrong prefix accepted'; exit 1; fi
if "$tool" good.tar dotdot ../top 2>/dev/null; then echo 'FAIL: invalid prefix accepted'; exit 1; fi
"$tool" good.tar plain
[ -f plain/data/top/etc/x ] || { echo 'FAIL: unprefixed import changed'; exit 1; }
echo 'FAKEFS_PREFIX_IMPORT_PASS strip hardlink symlink refuse-outside refuse-wrong-prefix refuse-dotdot unprefixed-unchanged'
