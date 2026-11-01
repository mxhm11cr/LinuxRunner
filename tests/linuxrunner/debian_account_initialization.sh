#!/bin/sh
# Fresh, disposable Debian root assembled by the current bootstrapper only.
# Real init and account tools; no replacement /etc/passwd or fake results.
set -eu
/usr/lib/linuxrunner/initialize-debian.sh
count=$(awk -F: '$1=="_apt" {n++} END {print n+0}' /etc/passwd)
[ "$count" = 1 ] || { echo "DEBIAN_ACCOUNT_FAIL duplicate _apt records=$count"; exit 1; }
/usr/lib/linuxrunner/initialize-debian.sh
[ "$(awk -F: '$1=="_apt" {n++} END {print n+0}' /etc/passwd)" = 1 ]
getent passwd _apt >/dev/null
dpkg --audit >/tmp/linuxrunner-dpkg-audit
[ ! -s /tmp/linuxrunner-dpkg-audit ]
echo DEBIAN_ACCOUNT_INITIALIZATION_PASS
