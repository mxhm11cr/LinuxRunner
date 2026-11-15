#!/bin/sh
# Run in a disposable Ubuntu guest after installing hello using signed APT.
set -eu
. /etc/os-release
[ "$ID" = ubuntu ]
[ "$(dpkg --print-architecture)" = arm64 ]
[ -r /usr/share/keyrings/ubuntu-archive-keyring.gpg ]
[ -s /etc/resolv.conf ]
audit=$(dpkg --audit)
[ -z "$audit" ]
dpkg-query -W -f='${Status}\n' hello | grep -qx 'install ok installed'
[ "$(hello)" = 'Hello, world!' ]
printf '%s\n' 'UBUNTU_BASE_CONTRACT_PASS architecture apt package resolver'
