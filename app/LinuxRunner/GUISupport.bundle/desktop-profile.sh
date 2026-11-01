#!/bin/sh
# LinuxRunner graphical desktop profile: authoritative check and targeted repair,
# run as root inside the guest by the app's desktop startup coordinator.
#   desktop-profile.sh check            report missing components, no network
#   desktop-profile.sh repair [accounts] install only what is missing
# Components:
#   gui       X server, VNC bridge, X11 build files, fonts; helpers built from
#             the current sources (focus-manager, gui-test, window-observer)
#   desktop   window manager, panel, file manager, D-Bus, xterm; desktop-probe;
#             desktop configuration
#   audio     PulseAudio server and recorder; audio-relay      (optional)
#   accounts  useradd/adduser, passwd, chpasswd                 (account setup)
# check writes profile-check.result: "ready" or "missing=<comma list>" and the
# optional ones as "optional-missing=<list>". repair prints PROFILE_STAGE lines
# (the app shows them) and only touches the network when packages are missing.
# Existing users, homes, installed applications and configuration are kept.
set -u
cd "$(dirname "$0")"
. ./support-common.sh
mode=${1:-check}
distro=$(sed -n 's/^ID=//p' /etc/os-release 2>/dev/null | tr -d '"')
have() { command -v "$1" >/dev/null 2>&1; }
helper_key() { { cat support-version; sha256sum "$@"; } | sha256sum | awk '{print $1}'; }

gui_missing() {
    for c in Xvfb x11vnc xauth fc-match gcc pkg-config readelf dbus-daemon; do have "$c" || return 0; done
    pkg-config --exists x11 xcb 2>/dev/null || return 0
    [ -x focus-manager ] && [ -x gui-test ] || return 0
    return 1
}
gui_helpers_stale() {
    have pkg-config || return 1
    key=$({ cat support-version; sha256sum gui-test.c focus-manager.c; pkg-config --modversion x11 xcb; } | sha256sum | awk '{print $1}')
    [ "$(cat gui-build.key 2>/dev/null || true)" != "$key" ]
}
desktop_commands_present() {
    have openbox && have xterm || return 1
    case "$distro" in
        arch|archarm) have pcmanfm-qt && have lxqt-panel;;
        *) have pcmanfm && { have lxpanel || have xfce4-panel; };;
    esac
}
desktop_missing() {
    desktop_commands_present || return 0
    [ -x desktop-probe ] || return 0
    [ "$(cat desktop-probe.key 2>/dev/null || true)" = "$(sha256sum desktop-probe.c | awk '{print $1}')" ] || return 0
    return 1
}
audio_missing() {
    for c in pulseaudio parec pactl; do have "$c" || return 0; done
    [ -x audio-relay ] || return 0
    return 1
}
accounts_missing() {
    have passwd || return 0
    have chpasswd || return 0
    have useradd || have adduser || return 0
    return 1
}

check() {
    missing= optional=
    if gui_missing || gui_helpers_stale; then missing=gui; fi
    if desktop_missing; then missing=${missing:+$missing,}desktop; fi
    if audio_missing; then optional=audio; fi
    if accounts_missing; then optional=${optional:+$optional,}accounts; fi
    { if [ -n "$missing" ]; then echo "missing=$missing"; else echo ready; fi
      [ -z "$optional" ] || echo "optional-missing=$optional"
      echo "distribution=${distro:-unknown}"
      case "$distro" in arch|archarm) echo desktop-components=openbox,lxqt-panel,pcmanfm-qt;; esac
      if have startplasma-x11 && have kwin_x11 && have plasmashell; then echo plasma=installed; else echo plasma=missing; fi
      if have startlxqt && have lxqt-panel && have pcmanfm-qt; then echo lxqt=installed; else echo lxqt=missing; fi
      if have enlightenment_start && have terminology; then echo enlightenment=installed; else echo enlightenment=missing; fi
      if have wayfire && have wf-panel && have foot; then echo wayfire=installed; else echo wayfire=missing; fi
      if have Hyprland && have waybar && have fuzzel && have seatd; then echo hyprland=installed; else echo hyprland=missing; fi
      # Startup timeline (app diagnostics): Debian first start, helper build.
      if [ -e /usr/lib/linuxrunner/initialize-debian.sh ] || [ -e /usr/lib/linuxrunner/initialize-arch.sh ]; then
          if [ -f /var/lib/linuxrunner/initialized ]; then echo first-start=done; else echo first-start=pending; fi
      fi
      if [ ! -x focus-manager ] || [ ! -x gui-test ]; then echo helpers=missing; elif gui_helpers_stale; then echo helpers=stale; else echo helpers=current; fi
    } > profile-check.result.tmp
    mv profile-check.result.tmp profile-check.result
    cat profile-check.result
}

stage() { echo "PROFILE_STAGE: $1"; }
apt_log=/tmp/linuxrunner-profile-apt.$$
# The real reason of a package failure, from the package manager's output:
#   PROFILE_FAILURE: category=<CATEGORY> stage=<what> detail=<first matching line>
classify_failure() {
    what=$1; text=$(cat "$apt_log" 2>/dev/null)
    # apt-get update succeeds with only warnings when downloads fail (offline,
    # certificate); the install then reports "Unable to locate package".
    # Report the download error instead.
    if printf '%s\n' "$text" | grep -qE 'Unable to locate package|has no installation candidate' &&
        grep -qE '^(Err:|W: Failed to fetch|E: Failed to fetch)' "$apt_log.update" 2>/dev/null; then
        text=$(grep -E '^(Err:|W: |E: )|^  ' "$apt_log.update"; printf '%s\n' "$text")
    fi
    first() { printf '%s\n' "$text" | grep -m 1 -E "$1"; }
    if line=$(first 'Could not get lock|frontend lock|is locked by|unable to lock database'); then category=PACKAGE_MANAGER_BUSY
    elif line=$(first 'certificate verify failed|Certificate verification failed|server certificate'); then
        category=TLS_CERTIFICATE_ERROR
        [ -s /etc/ssl/certs/ca-certificates.crt ] || line="$line (trust store /etc/ssl/certs/ca-certificates.crt missing: Debian setup unfinished)"
    elif line=$(first 'Temporary failure resolving|Could not resolve|Network is unreachable|Connection timed out|No route to host|Connection refused|failed retrieving file|failed to synchronize'); then category=NETWORK_UNAVAILABLE
    elif line=$(first 'No space left|You don.t have enough free space|Partition .* too full|not enough free disk space'); then category=INSUFFICIENT_STORAGE
    elif line=$(first 'Unable to locate package|has no installation candidate|target not found'); then category=PACKAGE_NOT_FOUND
    elif line=$(first 'Unmet dependencies|held broken packages|unmet dependenc|unable to satisfy dependency|conflicting dependencies|are in conflict'); then category=DEPENDENCY_CONFLICT
    elif line=$(first 'invalid or corrupted package|signature from .* is (unknown trust|invalid)|key .* could not be looked up'); then category=REPOSITORY_METADATA_ERROR
    elif line=$(first 'dpkg was interrupted|dpkg --configure -a'); then category=PACKAGE_DATABASE_RECOVERY_REQUIRED
    elif line=$(first 'InRelease|Release file|NO_PUBKEY|not signed|Hash Sum mismatch|404'); then category=REPOSITORY_METADATA_ERROR
    elif line=$(first 'Read-only file system|Input/output error|Permission denied'); then category=FILESYSTEM_ERROR
    else category=UNKNOWN; line=$(printf '%s\n' "$text" | grep -E '^(E|W): ' | tail -n 1); fi
    echo "PROFILE_FAILURE: category=$category stage=$what detail=$line"
}

install_packages() {
    # $1 = label, rest = Debian packages -- Alpine packages -- Arch Linux ARM packages
    label=$1; shift
    debian= alpine= arch= side=debian
    for p in "$@"; do if [ "$p" = -- ]; then if [ $side = debian ]; then side=alpine; else side=arch; fi; continue; fi
        case $side in debian) debian="$debian $p";; alpine) alpine="$alpine $p";; *) arch="$arch $p";; esac; done
    if have apt-get; then
        export DEBIAN_FRONTEND=noninteractive
        # installer-run.sh's package_lock_acquire already waited for Debian's
        # first-start configuration and other package operations; apt also
        # waits for dpkg's lock (90linuxrunner-lock-wait).
        # apt's machine-readable progress (dlstatus/pmstatus: real percentages)
        # goes to apt-status in the support directory; the app shows it.
        status_file="$PWD/apt-status"
        if [ -z "${apt_updated:-}" ]; then
            if dpkg --audit 2>/dev/null | grep -q .; then stage "Finishing an interrupted installation"; dpkg --configure -a >> "$apt_log" 2>&1 || { classify_failure "$label"; return 1; }; fi
            # Package lists from the last 3 days that can resolve these packages
            # are reused (a failed download below refreshes them and retries).
            stage "Checking package dependencies"
            if [ -n "$(find /var/lib/apt/lists -maxdepth 1 -name '*_InRelease' -mtime -3 2>/dev/null | head -n 1)" ] &&
                apt-get -s install --no-install-recommends $debian > "$apt_log" 2>&1; then
                apt_updated=reused; echo 'PROFILE: package lists recent; reused'
            else
                stage "Downloading package lists"
                : > "$status_file"
                apt-get -o APT::Status-Fd=3 update 3> "$status_file" > "$apt_log" 2>&1 || { cat "$apt_log"; classify_failure "package lists"; return 1; }
                cat "$apt_log"; cp "$apt_log" "$apt_log.update"; apt_updated=1
            fi
        fi
        stage "Installing $label"
        : > "$status_file"
        if ! apt-get -o APT::Status-Fd=3 install -y --no-install-recommends $debian 3> "$status_file" > "$apt_log" 2>&1; then
            # Reused lists can name a version the mirror no longer has: refresh once.
            if [ "$apt_updated" = reused ] && grep -qE '^(Err:|E: Failed to fetch)|404' "$apt_log"; then
                cat "$apt_log"; stage "Downloading package lists"; : > "$status_file"
                apt-get -o APT::Status-Fd=3 update 3> "$status_file" > "$apt_log" 2>&1 || { cat "$apt_log"; classify_failure "package lists"; return 1; }
                cat "$apt_log"; cp "$apt_log" "$apt_log.update"; apt_updated=1
                stage "Installing $label"; : > "$status_file"
                apt-get -o APT::Status-Fd=3 install -y --no-install-recommends $debian 3> "$status_file" > "$apt_log" 2>&1 || { cat "$apt_log"; classify_failure "$label"; return 1; }
            else cat "$apt_log"; classify_failure "$label"; return 1; fi
        fi
        cat "$apt_log"
    elif have pacman; then
        [ -n "$arch" ] || { echo "PROFILE_FAILURE: category=UNKNOWN stage=$label detail=no Arch Linux ARM package list"; return 1; }
        # pacman's per-package lines become the apt-status records the app shows
        # as real percentages: "(3/12) installing foo" -> pmstatus, downloads -> dlstatus.
        status_file="$PWD/apt-status"
        # Refresh and upgrade in the SAME transaction as the requested packages:
        # -Sy followed by -S leaves rolling Arch vulnerable to partial upgrades.
        pacman_refresh=
        if [ -z "${pacman_synced:-}" ]; then pacman_refresh=yu; fi
        stage "Installing $label${pacman_refresh:+ (synchronizing and upgrading Arch)}"
        : > "$status_file"
        { pacman -S$pacman_refresh --needed --noconfirm --noprogressbar $arch 2>&1; echo "PACMAN_EXIT:$?"; } | tee "$apt_log" | awk -v status="$status_file" '
            /^Packages \([0-9]+\)/ { gsub(/[^0-9]/, "", $2); total = $2 + 0 }
            / downloading\.\.\.$/ { d++; if (total) { printf "dlstatus:%d:%.1f:%s\n", d, 100 * d / total, $1 >> status; fflush(status) } }
            /^\( *[0-9]+\/[0-9]+\) installing / { split($0, a, /[()\/]/); n = a[2] + 0; t = a[3] + 0; name = $NF
              if (t) { printf "pmstatus:%s:%.1f:Installing %s\n", name, 100 * n / t, name >> status; fflush(status) } }'
        grep -q '^PACMAN_EXIT:0$' "$apt_log" || { cat "$apt_log"; classify_failure "$label"; return 1; }
        pacman_synced=1
        cat "$apt_log"
    elif have apk; then
        stage "Installing $label"
        apk add --no-cache $alpine > "$apt_log" 2>&1 || { cat "$apt_log"; classify_failure "$label"; return 1; }
        cat "$apt_log"
    else support_fail PROFILE package-manager-unsupported; fi
}

build_gui_helpers() {
    stage "Building display helpers"
    key=$({ cat support-version; sha256sum gui-test.c focus-manager.c; pkg-config --modversion x11 xcb; } | sha256sum | awk '{print $1}')
    gcc gui-test.c -O2 -Wall -Wextra -lX11 -o gui-test.new && gcc focus-manager.c -O2 -Wall -Wextra -lX11 -o focus-manager.new || return 1
    chmod 755 gui-test.new focus-manager.new
    verify_elf PROFILE "$SUPPORT_DIR/gui-test.new" && verify_elf PROFILE "$SUPPORT_DIR/focus-manager.new" || return 1
    mv gui-test.new gui-test; mv focus-manager.new focus-manager; echo "$key" > gui-build.key
    /bin/sh ./prepare-window-observer.sh || true
}
build_desktop_probe() {
    key=$(sha256sum desktop-probe.c | awk '{print $1}')
    [ -x desktop-probe ] && [ "$(cat desktop-probe.key 2>/dev/null || true)" = "$key" ] && return 0
    gcc desktop-probe.c -O2 -Wall -Wextra -lX11 -o desktop-probe.new || return 1
    chmod 755 desktop-probe.new; verify_elf PROFILE "$SUPPORT_DIR/desktop-probe.new" || return 1
    mv desktop-probe.new desktop-probe; echo "$key" > desktop-probe.key
}
build_audio_relay() {
    key=$(sha256sum audio-relay.c | awk '{print $1}')
    [ -x audio-relay ] && [ "$(cat audio-relay.key 2>/dev/null || true)" = "$key" ] && return 0
    gcc audio-relay.c -O2 -Wall -Wextra -o audio-relay.new || return 1
    chmod 755 audio-relay.new; verify_elf PROFILE "$SUPPORT_DIR/audio-relay.new" || return 1
    mv audio-relay.new audio-relay; echo "$key" > audio-relay.key
}

repair() {
    want_accounts=${1:-}
    verify_resources
    [ "$(id -u)" = 0 ] || support_fail PROFILE root-required
    if gui_missing; then
        install_packages "X server and display support" busybox procps xvfb x11vnc xauth x11-xserver-utils x11-utils x11-apps libx11-dev libxcb1-dev build-essential pkg-config binutils xterm xfonts-base fonts-dejavu-core fontconfig dbus-daemon dbus-x11 librsvg2-common \
            -- xvfb x11vnc xclock libx11-dev libxcb-dev build-base pkgconf binutils xterm font-misc-misc font-dejavu fontconfig xauth setxkbmap xmodmap dbus \
            -- procps-ng xorg-server-xvfb x11vnc xorg-xauth xorg-xset xorg-xrandr xorg-xprop xorg-xwininfo xorg-xdpyinfo xorg-xsetroot xorg-xmodmap xorg-setxkbmap xorg-xclock libx11 libxcb gcc make pkgconf binutils xterm xorg-fonts-misc ttf-dejavu fontconfig dbus librsvg || support_fail PROFILE gui-packages
        mkdir -p /var/lib/dbus; dbus-uuidgen --ensure 2>/dev/null || true
    fi
    if gui_missing || gui_helpers_stale; then build_gui_helpers || support_fail PROFILE gui-helpers; fi
    if desktop_missing; then
        if ! desktop_commands_present; then
            install_packages "desktop (window manager, panel, file manager)" openbox lxpanel pcmanfm adwaita-icon-theme fonts-dejavu-core dbus-daemon dbus-bin xterm \
                -- openbox xfce4-panel pcmanfm adwaita-icon-theme gdk-pixbuf-loaders font-dejavu dbus xterm \
                -- openbox lxqt-panel pcmanfm-qt adwaita-icon-theme ttf-dejavu dbus xterm || support_fail PROFILE desktop-packages
        fi
        stage "Building desktop helpers"
        build_desktop_probe || support_fail PROFILE desktop-probe
    fi
    stage "Configuring desktop"
    mkdir -p /var/lib/dbus; dbus-uuidgen --ensure 2>/dev/null || true
    /bin/sh ./desktop-configure.sh || support_fail PROFILE desktop-configure
    # Optional: sound. Failure leaves the desktop usable without sound.
    if audio_missing; then
        if ! { have pulseaudio && have parec && have pactl; }; then
            install_packages "sound" pulseaudio pulseaudio-utils libasound2-plugins -- pulseaudio pulseaudio-utils alsa-plugins-pulse -- pulseaudio libpulse pulseaudio-alsa || echo 'PROFILE: sound packages unavailable; desktop continues without sound'
        fi
        if have parec; then stage "Preparing sound"; build_audio_relay || echo 'PROFILE: sound relay build failed; desktop continues without sound'; fi
    fi
    # Account tools: LinuxRunner's Debian image does not include passwd.
    if [ "$want_accounts" = accounts ] && accounts_missing; then
        install_packages "account tools" passwd -- shadow -- shadow || support_fail PROFILE account-tools
    fi
    stage "Checking"
    check
    case "$(head -n 1 profile-check.result)" in ready) echo 'PROFILE: result=PASS';; *) support_fail PROFILE "still-$(head -n 1 profile-check.result)";; esac
}

case "$mode" in
    check) check;;
    repair) repair "${2:-}";;
    accounts)
        # Account tools only (and sudo when asked): native account setup.
        [ "$(id -u)" = 0 ] || support_fail PROFILE root-required
        stage "Checking existing installation"
        if accounts_missing; then install_packages "account tools" passwd -- shadow -- shadow || support_fail PROFILE account-tools; else echo 'PROFILE: account tools already installed'; fi
        if [ "${2:-}" = sudo ] && ! have sudo; then install_packages "administrator tools" sudo -- sudo -- sudo || support_fail PROFILE sudo; fi
        # Present is not enough: each tool must run.
        stage "Verifying installation"
        accounts_missing && support_fail PROFILE account-tools-missing
        for tool in useradd chpasswd passwd; do "$tool" --help > /dev/null 2>&1 || { echo "PROFILE_FAILURE: category=ACCOUNT_TOOL_BROKEN stage=verify detail=$tool --help failed"; support_fail PROFILE account-tool-broken; }; done
        if [ "${2:-}" = sudo ]; then sudo -V > /dev/null 2>&1 || { echo 'PROFILE_FAILURE: category=ACCOUNT_TOOL_BROKEN stage=verify detail=sudo -V failed'; support_fail PROFILE sudo-broken; }; fi
        echo 'PROFILE: accounts=PASS';;
    plasma)
        # Experimental KDE Plasma (X11 session) from Debian's own packages, on top
        # of the regular desktop (which stays available as the fallback).
        case "$distro" in
            debian) ;;
            arch|archarm)
                if have startplasma-x11 && have kwin_x11 && have plasmashell; then
                    repair "${2:-}"; echo 'PROFILE: plasma-packages=present session=not-tested'; exit 0
                fi
                echo 'PROFILE_FAILURE: category=PLASMA_X11_UNAVAILABLE stage=KDE detail=Arch rolling Plasma 6.8 no longer supplies startplasma-x11; LinuxRunner requires an X11 session. Use LinuxRunner Desktop.'
                support_fail PROFILE plasma-x11-unavailable;;
            *) support_fail PROFILE plasma-distribution-unsupported;;
        esac
        repair "${2:-}"
        if ! { have startplasma-x11 && have kwin_x11 && have plasmashell; }; then
            install_packages "KDE Plasma (experimental, about 550 packages)" plasma-desktop plasma-workspace kwin-x11 konsole dolphin breeze breeze-icon-theme dbus-x11 || support_fail PROFILE plasma-packages
        fi
        have startplasma-x11 && have kwin_x11 && have plasmashell || support_fail PROFILE plasma-missing
        echo 'PROFILE: plasma=PASS';;
    lxqt)
        # LXQt 2.x X11 session from the distribution's own packages, on top of
        # the regular desktop (which stays available as the fallback).
        repair "${2:-}"
        if ! { have startlxqt && have lxqt-panel && have pcmanfm-qt; }; then
            install_packages "LXQt" lxqt-core qterminal openbox lxqt-themes \
                -- lxqt-session lxqt-panel lxqt-runner lxqt-config pcmanfm-qt qterminal openbox lxqt-themes \
                -- lxqt-session lxqt-panel lxqt-runner lxqt-config pcmanfm-qt qterminal openbox lxqt-themes || support_fail PROFILE lxqt-packages
        fi
        have startlxqt && have lxqt-panel && have pcmanfm-qt || support_fail PROFILE lxqt-missing
        # LXQt's configuration names the Breeze icons, which Debian only
        # recommends: without them the menu, panel and file manager have no icons.
        # Also repairs an LXQt installed before this check existed.
        [ -d /usr/share/icons/breeze-dark ] || install_packages "Breeze icons" breeze-icon-theme -- breeze-icons -- breeze-icons || support_fail PROFILE lxqt-icons
        echo 'PROFILE: lxqt=PASS';;
    enlightenment)
        # Enlightenment (X11, its own software compositor) and the Terminology
        # terminal from the distribution's packages; the regular desktop stays
        # the fallback. Most of the download is enlightenment's theme and
        # wallpaper data. Not on Debian: its session stops at the splash screen.
        repair "${2:-}"
        case "$distro" in debian) support_fail PROFILE enlightenment-not-available-on-debian;; esac
        if ! { have enlightenment_start && have terminology; }; then
            install_packages "Enlightenment" enlightenment terminology \
                -- enlightenment terminology \
                -- enlightenment terminology || support_fail PROFILE enlightenment-packages
        fi
        have enlightenment_start && have terminology || support_fail PROFILE enlightenment-missing
        echo 'PROFILE: enlightenment=PASS';;
    wayfire)
        # Wayfire (wlroots), its wf-shell panel/dock/background, the foot terminal
        # and the WCM settings app from the distribution's packages. It runs
        # inside the session's X server with wlroots' software renderer.
        repair "${2:-}"
        [ "$distro" != alpine ] || support_fail PROFILE wayfire-unavailable-on-alpine
        if ! { have wayfire && have wf-panel && have foot; }; then
            install_packages "Wayfire" wayfire wf-shell foot wcm xwayland \
                -- wayfire \
                -- wayfire wf-shell foot wcm xorg-xwayland || support_fail PROFILE wayfire-packages
        fi
        have wayfire && have wf-panel && have foot || support_fail PROFILE wayfire-missing
        echo 'PROFILE: wayfire=PASS';;
    hyprland)
        # Hyprland on LinuxRunner's software KMS device (llvmpipe, no GPU), with
        # Waybar, the fuzzel launcher, the foot terminal and seatd. Arch Linux
        # ARM packages it; Debian 13 does not.
        repair "${2:-}"
        case "$distro" in arch|archarm) ;; *) support_fail PROFILE hyprland-needs-arch;; esac
        if ! { have Hyprland && have waybar && have fuzzel && have seatd; }; then
            install_packages "Hyprland" hyprland -- hyprland \
                -- hyprland waybar fuzzel foot seatd otf-font-awesome || support_fail PROFILE hyprland-packages
        fi
        have Hyprland && have waybar && have fuzzel && have seatd || support_fail PROFILE hyprland-missing
        # Appearance of the LinuxRunner preset: wallpaper (swaybg), icons and
        # fonts. Optional: without them the desktop works with a plain background.
        if ! { have swaybg && [ -d /usr/share/icons/Papirus-Dark ] && [ -d /usr/share/fonts/inter ]; }; then
            install_packages "Hyprland appearance" swaybg -- swaybg -- \
                swaybg papirus-icon-theme inter-font ttf-jetbrains-mono || echo 'PROFILE: hyprland appearance packages unavailable; plain background'
        fi
        echo 'PROFILE: hyprland=PASS';;
    *) exit 2;;
esac
