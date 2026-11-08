#!/bin/sh
# Uses LinuxRunner's socket layer and the resolver supplied by iPadOS.
# This check reads network state; it does not install or configure drivers.
set -eu
. /etc/os-release
[ "$ID" = steamos ]
getent ahostsv4 archlinuxarm.org >/dev/null
printf 'STEAMOS_NETWORK_DNS=PASS\n'
curl --fail --silent --show-error --location --proto '=https' --proto-redir '=https' \
    --connect-timeout 15 --max-time 40 --output /dev/null https://archlinuxarm.org/
printf 'STEAMOS_NETWORK_HTTPS=PASS\n'
