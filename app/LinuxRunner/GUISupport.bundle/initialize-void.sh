#!/bin/sh
# Void Linux first start. Called by the app's PID 1 and by managed package
# preflight. No runit boot; the official Void ROOTFS and its signed xbps
# repository configuration are kept.
set -eu
umask 022
mkdir -p /var/lib/linuxrunner /var/log /run /tmp /root
chmod 1777 /tmp
exec 9>/var/lib/linuxrunner/void-initialize.lock
flock -x 9
. /etc/os-release
[ "$ID" = void ] || { echo 'VOID_INIT: FAIL wrong distribution'; exit 1; }
[ -f /var/lib/linuxrunner/initialized ] && exit 0
[ "$(xbps-uhelper arch)" = aarch64 ] || { echo 'VOID_INIT: FAIL wrong package architecture'; exit 1; }
for tool in /bin/sh /usr/bin/xbps-install /usr/bin/xbps-query; do
    [ -x "$tool" ] || { echo "VOID_INIT: FAIL required executable missing: $tool"; exit 1; }
done
ls /var/db/xbps/keys/*.plist >/dev/null 2>&1 || { echo 'VOID_INIT: FAIL repository signing keys missing'; exit 1; }
[ -s /etc/resolv.conf ] || printf 'nameserver 1.1.1.1\nnameserver 8.8.8.8\n' > /etc/resolv.conf
# The image is shared by every download: give this installation its own
# machine identity (D-Bus and others key on it).
if [ ! -f /var/lib/linuxrunner/machine-id-regenerated ]; then
    rm -f /etc/machine-id.new
    od -An -N16 -tx1 /dev/urandom | tr -d ' \n' > /etc/machine-id.new
    [ "$(wc -c < /etc/machine-id.new)" -eq 32 ] || { echo 'VOID_INIT: FAIL machine-id generation'; exit 1; }
    echo >> /etc/machine-id.new; chmod 0444 /etc/machine-id.new; mv /etc/machine-id.new /etc/machine-id
    mkdir -p /var/lib/dbus; ln -sf /etc/machine-id /var/lib/dbus/machine-id
    : > /var/lib/linuxrunner/machine-id-regenerated
fi
# The ROOTFS ships Void's publicly documented root password. Lock it (files
# are kept); LinuxRunner's account step sets up the user's own account.
hash=$(grep '^root:' /etc/shadow | cut -d: -f2)
case "$hash" in '!'*|'*'*|'') ;; *) passwd -l root >/dev/null && echo 'VOID_INIT: locked preset password of root';; esac
printf 'distribution=void\nbase_validation=passed\n' > /var/lib/linuxrunner/initialized.new
mv /var/lib/linuxrunner/initialized.new /var/lib/linuxrunner/initialized
echo 'VOID_INIT: PASS'
