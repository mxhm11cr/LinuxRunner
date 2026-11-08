#!/bin/sh
# Called by the app's PID 1 and by managed package preflight. No systemd boot,
# account replacement, package database reconstruction, or system upgrade.
set -eu
umask 022
mkdir -p /var/lib/linuxrunner /var/log /run /tmp /root
chmod 1777 /tmp
# Inherited descriptor keeps competing first-start checks serialized. DNF/RPM
# still own their normal transaction locks for all package modifications.
exec 9>/var/lib/linuxrunner/fedora-initialize.lock
flock -x 9
. /etc/os-release
[ "$ID" = fedora ] || { echo 'FEDORA_INIT: FAIL wrong distribution'; exit 1; }
if [ -f /var/lib/linuxrunner/initialized ] && command -v awk >/dev/null 2>&1; then exit 0; fi
# Revalidate an older first-start checkpoint that predates the required tools.
# Only LinuxRunner's own readiness record moves; package metadata stays intact.
if [ -f /var/lib/linuxrunner/initialized ]; then
    mv /var/lib/linuxrunner/initialized /var/lib/linuxrunner/initialized.previous
fi
[ "$(rpm --eval '%{_arch}')" = aarch64 ] || { echo 'FEDORA_INIT: FAIL wrong package architecture'; exit 1; }
for tool in /bin/bash /usr/bin/dnf /usr/bin/rpm /usr/bin/rpmdb; do
    [ -x "$tool" ] || { echo "FEDORA_INIT: FAIL required executable missing: $tool"; exit 1; }
done
echo 'FEDORA_INIT: checking the existing RPM database and dependencies'
rpmdb --verifydb
dnf check
# The official minimal image omits awk. Package coordination, inventory and
# support verification use it before the desktop dependency installer can run.
# Install the distribution's signed tool rather than relying on a GUI package
# incidentally pulling it in. Failed/interrupted installs leave no ready marker.
if ! command -v awk >/dev/null 2>&1; then
    echo 'FEDORA_INIT: installing required package-management tools (network needed)'
    dnf -y --setopt=install_weak_deps=False install gawk
    command -v awk >/dev/null
    rpmdb --verifydb
    dnf check
fi
# Do not replace the official image's accounts, DNS, repository trust or keys.
# A temporary marker is renamed only after both real package checks succeed.
printf 'distribution=fedora\nbase_validation=passed\n' > /var/lib/linuxrunner/initialized.new
mv /var/lib/linuxrunner/initialized.new /var/lib/linuxrunner/initialized
echo 'FEDORA_INIT: PASS'
