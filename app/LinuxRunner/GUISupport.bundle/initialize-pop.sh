#!/bin/sh
# Pop!_OS 24.04 ARM64 on LinuxRunner: Ubuntu 24.04 Base (Canonical's signed,
# SHA-256-pinned image) plus System76's own Pop!_OS repository, as Pop!_OS
# itself is Ubuntu with Pop's archive layered on top.
#   * Runs initialize-ubuntu.sh first (same base validation as Ubuntu).
#   * Installs Pop's archive key (fingerprint below, shipped in this bundle)
#     and the repository restricted to that key (Signed-By).
#   * Pins Pop's archive above Ubuntu's, as Pop!_OS does, so Pop's builds win.
#   * Installs pop-default-settings (Pop identity, defaults) without
#     recommends. The desktop (COSMIC) is a separate, explicit install.
# Idempotent; any failure leaves the Ubuntu base usable and is reported.
set -eu
umask 022
cd "$(dirname "$0")"
# Pop OS (ISO Signing Key) <info@system76.com>, 63C46DF0140D738961429F4E204DD8AEC33A7AFF
mkdir -p /var/lib/linuxrunner
exec 8>/var/lib/linuxrunner/pop-initialize.lock
flock -x 8
if [ -f /var/lib/linuxrunner/pop-initialized ]; then exit 0; fi
if [ ! -f /var/lib/linuxrunner/initialized ]; then
    /bin/sh ./initialize-ubuntu.sh || { echo 'POP_INIT: FAIL Ubuntu base validation'; exit 1; }
fi
[ -r ./pop-archive-keyring.gpg ] || { echo 'POP_INIT: FAIL Pop archive key missing from support bundle'; exit 1; }
install -m 0644 ./pop-archive-keyring.gpg /usr/share/keyrings/pop-archive-keyring.gpg
cat > /etc/apt/sources.list.d/pop-os-release.sources.new <<EOF
X-Repolib-Name: Pop_OS Release Sources
Enabled: yes
Types: deb
URIs: http://apt.pop-os.org/release
Suites: noble
Components: main
Signed-By: /usr/share/keyrings/pop-archive-keyring.gpg
EOF
mv /etc/apt/sources.list.d/pop-os-release.sources.new /etc/apt/sources.list.d/pop-os-release.sources
cat > /etc/apt/preferences.d/pop-default-settings.new <<'EOF'
# Pop!_OS builds take precedence over Ubuntu's (as on Pop!_OS itself).
Package: *
Pin: origin apt.pop-os.org
Pin-Priority: 1001
EOF
mv /etc/apt/preferences.d/pop-default-settings.new /etc/apt/preferences.d/pop-default-settings
export DEBIAN_FRONTEND=noninteractive
echo 'POP_INIT: refreshing package lists (Ubuntu and Pop!_OS)'
apt-get update || { echo 'POP_INIT: FAIL apt-get update (network or signature)'; exit 1; }
echo 'POP_INIT: installing Pop!_OS defaults'
# Pop!_OS ships its own builds of some Ubuntu packages (libsystemd0, libudev1)
# at lower versions; the 1001 pin selects them, which apt reports as downgrades.
apt-get install -y --allow-downgrades --no-install-recommends pop-default-settings pop-gtk-theme pop-icon-theme \
    || { echo 'POP_INIT: FAIL installing pop-default-settings'; exit 1; }
. /etc/os-release
echo "POP_INIT: os-release ID=$ID NAME=$NAME"
printf 'distribution=pop\nbase=ubuntu-24.04\n' > /var/lib/linuxrunner/pop-initialized
echo 'POP_INIT: PASS'
