#!/bin/sh
# Run on a disposable, already initialized Ubuntu guest with the deployed
# support bundle as $1. Existing identities and repository trust must survive.
set -eu
initializer=${1:?supply path to initialize-ubuntu.sh}
[ -f /var/lib/linuxrunner/initialized ]
grep -qx 'distribution=ubuntu' /var/lib/linuxrunner/initialized
before=$(sha256sum /etc/passwd /etc/group /etc/machine-id /usr/share/keyrings/ubuntu-archive-keyring.gpg /var/lib/linuxrunner/initialized)
/bin/sh "$initializer"
after=$(sha256sum /etc/passwd /etc/group /etc/machine-id /usr/share/keyrings/ubuntu-archive-keyring.gpg /var/lib/linuxrunner/initialized)
[ "$before" = "$after" ]
printf '%s\n' 'UBUNTU_INIT_IDEMPOTENCE_PASS accounts machine-id archive-key marker'
