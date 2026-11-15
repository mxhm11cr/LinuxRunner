#!/bin/sh
# Validate the official Ubuntu userspace without booting systemd or replacing
# its accounts, package database, repository configuration, or archive keys.
set -eu
umask 022
mkdir -p /var/lib/linuxrunner /var/log /run /tmp /root
chmod 1777 /tmp
exec 9>/var/lib/linuxrunner/ubuntu-initialize.lock
flock -x 9
. /etc/os-release
# Pop!_OS and Zorin OS are layered on this Ubuntu base (initialize-pop.sh, initialize-zorin.sh).
case "$ID" in ubuntu|pop|zorin) ;; *) echo 'UBUNTU_INIT: FAIL wrong distribution'; exit 1;; esac
[ -f /var/lib/linuxrunner/initialized ] && exit 0
[ "$(dpkg --print-architecture)" = arm64 ] || { echo 'UBUNTU_INIT: FAIL wrong package architecture'; exit 1; }
for tool in /bin/bash /usr/bin/apt-get /usr/bin/dpkg; do
    [ -x "$tool" ] || { echo "UBUNTU_INIT: FAIL required executable missing: $tool"; exit 1; }
done
[ -r /usr/share/keyrings/ubuntu-archive-keyring.gpg ] || { echo 'UBUNTU_INIT: FAIL archive keyring missing'; exit 1; }
audit=$(dpkg --audit) || { echo 'UBUNTU_INIT: FAIL dpkg audit failed'; exit 1; }
[ -z "$audit" ] || { echo 'UBUNTU_INIT: FAIL package database needs repair'; printf '%s\n' "$audit"; exit 1; }
if [ ! -s /etc/machine-id ]; then
    od -An -N16 -tx1 /dev/urandom | tr -d ' \n' > /etc/machine-id.new
    [ "$(wc -c < /etc/machine-id.new)" -eq 32 ] || { echo 'UBUNTU_INIT: FAIL machine-id generation'; exit 1; }
    echo >> /etc/machine-id.new
    chmod 0444 /etc/machine-id.new
    mv /etc/machine-id.new /etc/machine-id
fi
printf 'distribution=ubuntu\nbase_validation=passed\n' > /var/lib/linuxrunner/initialized.new
mv /var/lib/linuxrunner/initialized.new /var/lib/linuxrunner/initialized
echo 'UBUNTU_INIT: PASS'
