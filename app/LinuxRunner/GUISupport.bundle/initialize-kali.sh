#!/bin/sh
# Kali Linux first start. Called by the app's PID 1 and by managed package
# preflight. No systemd boot and no package database reconstruction; the
# official Kali userspace and its signed archive configuration are kept.
set -eu
umask 022
mkdir -p /var/lib/linuxrunner /var/log /run /tmp /root
chmod 1777 /tmp
# Serializes competing first-start checks; dpkg/apt keep their own locks.
exec 9>/var/lib/linuxrunner/kali-initialize.lock
flock -x 9
. /etc/os-release
[ "$ID" = kali ] || { echo 'KALI_INIT: FAIL wrong distribution'; exit 1; }
[ -f /var/lib/linuxrunner/initialized ] && exit 0
[ "$(dpkg --print-architecture)" = arm64 ] || { echo 'KALI_INIT: FAIL wrong package architecture'; exit 1; }
for tool in /bin/bash /usr/bin/apt-get /usr/bin/dpkg; do
    [ -x "$tool" ] || { echo "KALI_INIT: FAIL required executable missing: $tool"; exit 1; }
done
[ -r /usr/share/keyrings/kali-archive-keyring.gpg ] || { echo 'KALI_INIT: FAIL Kali archive keyring missing'; exit 1; }
echo 'KALI_INIT: checking the existing package database'
# dpkg --audit prints packages left half-installed/configured; any output fails.
audit=$(dpkg --audit 2>&1) || true
[ -z "$audit" ] || { echo "KALI_INIT: FAIL package database needs repair"; echo "$audit" | head -n 20; exit 1; }
# The image is shared by every download: give this installation its own
# machine identity (D-Bus and others key on it).
if [ ! -f /var/lib/linuxrunner/machine-id-regenerated ]; then
    rm -f /etc/machine-id.new
    od -An -N16 -tx1 /dev/urandom | tr -d ' \n' > /etc/machine-id.new
    [ "$(wc -c < /etc/machine-id.new)" -eq 32 ] || { echo 'KALI_INIT: FAIL machine-id generation'; exit 1; }
    echo >> /etc/machine-id.new; chmod 0444 /etc/machine-id.new; mv /etc/machine-id.new /etc/machine-id
    mkdir -p /var/lib/dbus; ln -sf /etc/machine-id /var/lib/dbus/machine-id
    : > /var/lib/linuxrunner/machine-id-regenerated
fi
# The image's preset root and kali passwords are publicly documented. Lock
# them (accounts, homes and files are kept); LinuxRunner's account step sets
# up the user's own account and password.
for account in root kali; do
    if getent passwd "$account" >/dev/null; then
        hash=$(getent shadow "$account" | cut -d: -f2)
        case "$hash" in '!'*|'*'*|'') ;; *) passwd -l "$account" >/dev/null && echo "KALI_INIT: locked preset password of $account";; esac
    fi
done
printf 'distribution=kali\nbase_validation=passed\n' > /var/lib/linuxrunner/initialized.new
mv /var/lib/linuxrunner/initialized.new /var/lib/linuxrunner/initialized
echo 'KALI_INIT: PASS'
