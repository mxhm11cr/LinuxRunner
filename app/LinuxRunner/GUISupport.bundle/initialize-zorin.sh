#!/bin/sh
# Zorin OS 18 ARM64 on LinuxRunner: Ubuntu 24.04 Base (Canonical's signed,
# SHA-256-pinned image) plus Zorin OS's own Launchpad PPAs (stable, patches,
# apps), which carry arm64 builds of Zorin's desktop, settings and themes.
#   * Runs initialize-ubuntu.sh first (same base validation as Ubuntu).
#   * Installs the PPAs' signing key ("Launchpad PPA for Zorin OS",
#     B2228B8CCA5C00A2C59D35E530FCF8F64F71B61C, shipped in this bundle) and
#     restricts the repositories to it (Signed-By).
#   * Pins Zorin's builds above Ubuntu's, so Zorin's patched packages win.
#   * Installs Zorin's defaults and appearance without recommends. The Zorin
#     desktop itself (GNOME with Zorin's layout) is a separate, explicit install.
# Idempotent; any failure leaves the Ubuntu base usable and is reported.
set -eu
umask 022
cd "$(dirname "$0")"
mkdir -p /var/lib/linuxrunner
exec 8>/var/lib/linuxrunner/zorin-initialize.lock
flock -x 8
if [ -f /var/lib/linuxrunner/zorin-initialized ]; then exit 0; fi
if [ ! -f /var/lib/linuxrunner/initialized ]; then
    /bin/sh ./initialize-ubuntu.sh || { echo 'ZORIN_INIT: FAIL Ubuntu base validation'; exit 1; }
fi
[ -r ./zorin-archive-keyring.gpg ] || { echo 'ZORIN_INIT: FAIL Zorin archive key missing from support bundle'; exit 1; }
export DEBIAN_FRONTEND=noninteractive
# Ubuntu Base has no CA certificates; Launchpad's PPAs are HTTPS-only. Install
# them from Ubuntu's own signed archive (http, apt-verified) first.
if [ ! -s /etc/ssl/certs/ca-certificates.crt ]; then
    echo 'ZORIN_INIT: installing CA certificates from Ubuntu'
    rm -f /etc/apt/sources.list.d/zorin.sources   # HTTPS source cannot work yet
    apt-get update || { echo 'ZORIN_INIT: FAIL apt-get update (Ubuntu)'; exit 1; }
    apt-get install -y --no-install-recommends ca-certificates || { echo 'ZORIN_INIT: FAIL installing ca-certificates'; exit 1; }
fi
install -m 0644 ./zorin-archive-keyring.gpg /usr/share/keyrings/zorin-archive-keyring.gpg
cat > /etc/apt/sources.list.d/zorin.sources.new <<EOF
Types: deb
URIs: https://ppa.launchpadcontent.net/zorinos/stable/ubuntu https://ppa.launchpadcontent.net/zorinos/patches/ubuntu https://ppa.launchpadcontent.net/zorinos/apps/ubuntu
Suites: noble
Components: main
Signed-By: /usr/share/keyrings/zorin-archive-keyring.gpg
EOF
mv /etc/apt/sources.list.d/zorin.sources.new /etc/apt/sources.list.d/zorin.sources
cat > /etc/apt/preferences.d/zorin.new <<'EOF'
# Zorin OS builds take precedence over Ubuntu's (patched GNOME and settings).
Package: *
Pin: release o=LP-PPA-zorinos-stable
Pin-Priority: 1001

Package: *
Pin: release o=LP-PPA-zorinos-patches
Pin-Priority: 1001

Package: *
Pin: release o=LP-PPA-zorinos-apps
Pin-Priority: 600
EOF
mv /etc/apt/preferences.d/zorin.new /etc/apt/preferences.d/zorin
export DEBIAN_FRONTEND=noninteractive
echo 'ZORIN_INIT: refreshing package lists (Ubuntu and Zorin OS)'
apt-get update || { echo 'ZORIN_INIT: FAIL apt-get update (network or signature)'; exit 1; }
echo 'ZORIN_INIT: installing Zorin OS defaults and appearance'
# base-files and distro-info-data from Zorin's patches PPA carry the Zorin OS
# identity (/etc/os-release); the 1001 pin selects them over Ubuntu's.
apt-get install -y --allow-downgrades --no-install-recommends base-files distro-info-data zorin-os-default-settings zorin-desktop-themes zorin-icon-themes \
    || { echo 'ZORIN_INIT: FAIL installing Zorin OS defaults'; exit 1; }
. /etc/os-release
echo "ZORIN_INIT: os-release ID=$ID NAME=$NAME"
printf 'distribution=zorin\nbase=ubuntu-24.04\n' > /var/lib/linuxrunner/zorin-initialized
echo 'ZORIN_INIT: PASS'
