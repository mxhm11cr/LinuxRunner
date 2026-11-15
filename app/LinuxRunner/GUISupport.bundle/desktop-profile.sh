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
if [ "$distro" = steamos ]; then exec /bin/sh ./steamos-profile.sh "$@"; fi
# Kali Linux is Debian-based (same package names and apt); keep its identity
# for Kali-only profiles (its own Xfce desktop and tool metapackages).
# Pop!_OS and Zorin OS are Ubuntu with their vendor's repository layered on
# top (initialize-<flavor>.sh); Ubuntu checks and package names apply.
os_id=$distro
case "$distro" in pop|zorin) distro=ubuntu;; esac
distro_id=$distro
[ "$distro" != kali ] || distro=debian
[ "$distro" != ubuntu ] || distro=debian
have() { command -v "$1" >/dev/null 2>&1; }
gnome_packages_present() {
    for package in gnome-shell gnome-session ubuntu-session gnome-shell-extension-ubuntu-dock ubuntu-settings ubuntu-wallpapers yaru-theme-gnome-shell yaru-theme-gtk yaru-theme-icon nautilus gnome-terminal dbus-x11; do
        [ "$(dpkg-query -W -f='${db:Status-Status}' "$package" 2>/dev/null)" = installed ] || return 1
    done
}
helper_key() { { cat support-version; sha256sum "$@"; } | sha256sum | awk '{print $1}'; }

gui_missing() {
    # Fedora's minimal image/DejaVu outline fonts do not provide xterm's core
    # fixed font. Check the real file so existing profiles repair this too.
    if [ "$distro" = fedora ] && [ ! -r /usr/share/X11/fonts/misc/6x13.pcf.gz ]; then return 0; fi
    for c in Xvfb x11vnc xauth fc-match gcc pkg-config readelf dbus-daemon awk find; do have "$c" || return 0; done
    pkg-config --exists x11 xcb 2>/dev/null || return 0
    [ -x focus-manager ] && [ -x gui-test ] || return 0
    return 1
}
gui_helpers_stale() {
    have pkg-config && have awk || return 0
    key=$({ cat support-version; sha256sum gui-test.c focus-manager.c; pkg-config --modversion x11 xcb; } | sha256sum | awk '{print $1}')
    [ "$(cat gui-build.key 2>/dev/null || true)" != "$key" ]
}
desktop_commands_present() {
    have openbox && have xterm || return 1
    case "$distro" in
        arch|archarm) have pcmanfm-qt && have lxqt-panel;;
        # Fedora's lxpanel needs LXDE menu data (lxmenu-data): its own
        # applications.menu names categories from files Fedora does not
        # install, so the Applications menu showed blank categories.
        fedora) have pcmanfm && have lxpanel && [ -r /etc/xdg/menus/lxde-applications.menu ];;
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

# The RPM database's identity (size, mtime, inode of its files). A clean
# "dnf check" is reused until packages change; it costs seconds per start.
# Only the database files: even a read-only "dnf check" rewrites SQLite's
# -shm/-wal side files, which made every key differ (14 s per desktop start).
rpmdb_key() { stat -L -c '%n %s %Y %i' /var/lib/rpm/rpmdb.sqlite /var/lib/rpm/Packages 2>/dev/null | sha256sum | awk '{print $1}'; }
check() {
    missing= optional= helpers_stale=0
    gui_helpers_stale && helpers_stale=1
    if gui_missing || [ "$helpers_stale" = 1 ]; then missing=gui; fi
    if desktop_missing; then missing=${missing:+$missing,}desktop; fi
    # Executables can survive a failed RPM transaction while a required
    # dependency is absent. Do not turn that partial installation into ready.
    if [ "$distro" = fedora ] && have dnf; then
        key=$(rpmdb_key)
        if [ -n "$key" ] && [ "$(cat dnf-check.key 2>/dev/null || true)" = "$key" ]; then
            echo 'PROFILE: package-database=unchanged-since-last-clean-check'
        elif dnf check > "$apt_log.health" 2>&1; then
            [ -z "$key" ] || printf '%s\n' "$key" > dnf-check.key 2>/dev/null || true
        else
            rm -f dnf-check.key
            missing=${missing:+$missing,}package-database
            cat "$apt_log.health"
        fi
    fi
    if audio_missing; then optional=audio; fi
    if accounts_missing; then optional=${optional:+$optional,}accounts; fi
    { if [ -n "$missing" ]; then echo "missing=$missing"; else echo ready; fi
      [ -z "$optional" ] || echo "optional-missing=$optional"
      echo "distribution=${distro:-unknown}"
      case "$distro" in arch|archarm) echo desktop-components=openbox,lxqt-panel,pcmanfm-qt;; fedora) echo desktop-components=openbox,lxpanel,pcmanfm;; esac
      if have startplasma-x11 && have kwin_x11 && have plasmashell; then echo plasma=installed
      elif { [ "$distro" = arch ] || [ "$distro" = archarm ] || [ "$distro" = fedora ]; } && have kwin_wayland && have plasmashell && [ -x ./lr-plasma-bridge ]; then echo plasma=installed; echo plasma-backend=wayland
      else echo plasma=missing; fi
      if have startlxqt && have lxqt-panel && have pcmanfm-qt; then echo lxqt=installed; else echo lxqt=missing; fi
      if have enlightenment_start && have terminology; then echo enlightenment=installed; else echo enlightenment=missing; fi
      if have wayfire && have wf-panel && have foot; then echo wayfire=installed; else echo wayfire=missing; fi
      if have i3 && have xterm && have dmenu_run; then echo i3=installed; else echo i3=missing; fi
      if [ "$os_id" = zorin ]; then gnome_mode=zorin gnome_extension=zorin-taskbar@zorinos.com; else gnome_mode=ubuntu gnome_extension=ubuntu-dock@ubuntu.com; fi
      if have gnome-shell && have gnome-session && [ -x /usr/lib/systemd/systemd-logind ] && [ -f "/usr/share/gnome-shell/modes/$gnome_mode.json" ] && [ -d "/usr/share/gnome-shell/extensions/$gnome_extension" ]; then echo gnome=installed; else echo gnome=missing; fi
      if have Hyprland && have waybar && have fuzzel && have seatd; then echo hyprland=installed; else echo hyprland=missing; fi
      if have mate-session && have marco && have mate-panel; then echo mate=installed; else echo mate=missing; fi
      if have cinnamon-session && have cinnamon; then echo cinnamon=installed; else echo cinnamon=missing; fi
      if have budgie-desktop && have budgie-panel; then echo budgie=installed; else echo budgie=missing; fi
      if have cosmic-session && have cosmic-comp && have cosmic-panel; then echo cosmic=installed; else echo cosmic=missing; fi
      # Startup timeline: real initialization state, independent of package history.
      if [ "$distro" = fedora ] || [ "$distro_id" = ubuntu ] || [ -e /usr/lib/linuxrunner/initialize-debian.sh ] || [ -e /usr/lib/linuxrunner/initialize-arch.sh ]; then
          if [ -f /var/lib/linuxrunner/initialized ] && { [ "$distro" != fedora ] || have awk; }; then echo first-start=done; else echo first-start=pending; fi
      fi
      if [ ! -x focus-manager ] || [ ! -x gui-test ]; then echo helpers=missing; elif [ "$helpers_stale" = 1 ]; then echo helpers=stale; else echo helpers=current; fi
    } > profile-check.result.tmp
    mv profile-check.result.tmp profile-check.result
    cat profile-check.result
}

# Each step starts with an empty progress record (apt-status), so the app never
# shows the previous step's percentage under a new step.
stage() { : > "$SUPPORT_DIR/apt-status" 2>/dev/null || true; echo "PROFILE_STAGE: $1"; }
# Translate package-manager output into the apt-status records the app reads
# ("dlstatus|pmstatus:<item>:<percent>:<message>"), from the real counters each
# tool prints: dnf "[ n/m]", apk/zypper "(n/m) Installing", xbps configuring
# lines against its transaction size. Nothing is estimated.
progress_filter() { # $1 = tool
    awk -v tool="$1" -v status="$SUPPORT_DIR/apt-status" '
        function rec(kind, n, t, name, verb) { if (t > 0) { printf "%s:%s:%.1f:%s %s (%d of %d)\n", kind, name, 100 * n / t, verb, name, n, t >> status; fflush(status) } }
        { print; fflush() }
        tool == "dnf" && match($0, /^\[ *[0-9]+\/[0-9]+\]/) { split(substr($0, 2, RLENGTH - 2), c, "/"); n = c[1] + 0; t = c[2] + 0
            split(substr($0, RLENGTH + 1), w, " ")
            if (w[1] ~ /^(Installing|Upgrading|Reinstalling)$/) rec("pmstatus", n, t, w[2], "Installing"); else if (w[1] !~ /^(Verifying|Running|Prepare|Removing|Cleaning)/) rec("dlstatus", n, t, w[1], "Downloading") }
        tool == "dnf" && /^ +(Installing|Upgrading) +: / && match($0, /[0-9]+\/[0-9]+ *$/) { split(substr($0, RSTART), c, "/"); rec("pmstatus", c[1] + 0, c[2] + 0, $3, "Installing") }
        (tool == "apk" || tool == "zypper") && match($0, /^\( *[0-9]+\/[0-9]+\) (Installing|Upgrading)/) { split(substr($0, 2), c, /[\/)]/); name = $3; sub(/:$/, "", $2); rec("pmstatus", c[1] + 0, c[2] + 0, name, "Installing") }
        tool == "zypper" && /^Retrieving: / && match($0, /\( *[0-9]+\/[0-9]+\)/) { split(substr($0, RSTART + 1, RLENGTH - 2), c, "/"); rec("dlstatus", c[1] + 0, c[2] + 0, $2, "Downloading") }
        tool == "xbps" && /^[^ ]+ +(install|update|reinstall) +/ { total++ }
        tool == "xbps" && / configuring \.\.\.$/ { done++; name = $1; sub(/:$/, "", name); rec("pmstatus", done, total, name, "Installing") }
'
}
run_progress() { # $1 = tool, rest = command; keeps the command's exit status
    tool=$1; shift
    : > "$SUPPORT_DIR/apt-status" 2>/dev/null || true
    { "$@" 2>&1; echo "LR_PM_EXIT:$?"; } | progress_filter "$tool" > "$apt_log"
    grep -q '^LR_PM_EXIT:0$' "$apt_log"
}
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
    elif line=$(first 'Unable to locate package|has no installation candidate|target not found|No match for argument|Unable to find a match'); then category=PACKAGE_NOT_FOUND
    elif line=$(first 'Unmet dependencies|held broken packages|unmet dependenc|unable to satisfy dependency|conflicting dependencies|are in conflict|conflicting requests|nothing provides'); then category=DEPENDENCY_CONFLICT
    elif line=$(first 'invalid or corrupted package|signature from .* is (unknown trust|invalid)|key .* could not be looked up|GPG check FAILED|Public key for .* is not installed'); then category=REPOSITORY_METADATA_ERROR
    elif line=$(first 'dpkg was interrupted|dpkg --configure -a'); then category=PACKAGE_DATABASE_RECOVERY_REQUIRED
    elif line=$(first 'InRelease|Release file|NO_PUBKEY|not signed|Hash Sum mismatch|404'); then category=REPOSITORY_METADATA_ERROR
    elif line=$(first 'cap_set_file failed.*Operation not supported'); then category=FILESYSTEM_FEATURE_UNSUPPORTED
    elif line=$(first 'Read-only file system|Input/output error|Permission denied'); then category=FILESYSTEM_ERROR
    else category=UNKNOWN; line=$(printf '%s\n' "$text" | grep -E '^(E|W): ' | tail -n 1); fi
    echo "PROFILE_FAILURE: category=$category stage=$what detail=$line"
}

install_packages() {
    # $1 = label, then Debian -- Alpine -- Arch -- Fedora [-- Void [-- openSUSE]]
    # package names. Missing adapters fail explicitly; never pass another
    # distro's names or an empty request to its package manager.
    label=$1; shift
    debian= alpine= arch= fedora= void= suse= side=debian
    for p in "$@"; do
        if [ "$p" = -- ]; then
            case $side in debian) side=alpine;; alpine) side=arch;; arch) side=fedora;; fedora) side=void;; void) side=suse;; *) return 2;; esac
            continue
        fi
        case $side in debian) debian="$debian $p";; alpine) alpine="$alpine $p";; arch) arch="$arch $p";; fedora) fedora="$fedora $p";; void) void="$void $p";; suse) suse="$suse $p";; esac
    done
    if have apt-get; then
        export DEBIAN_FRONTEND=noninteractive
        # installer-run.sh's package_lock_acquire already waited for Debian's
        # first-start configuration and other package operations; apt also
        # waits for dpkg's lock (90linuxrunner-lock-wait).
        # apt's machine-readable progress (dlstatus/pmstatus: real percentages)
        # goes to apt-status in the support directory; the app shows it.
        status_file="$PWD/apt-status"
        # man-db rebuilds its whole index after every package batch, which
        # re-reads every manual page in the interpreter (minutes for Kali's
        # tools). Skip that rebuild; man still works, apropos may be stale.
        echo 'man-db man-db/auto-update boolean false' | debconf-set-selections 2>/dev/null || true
        if [ -z "${apt_updated:-}" ]; then
            if dpkg --audit 2>/dev/null | grep -q .; then
                stage "Finishing an interrupted installation"
                # dpkg finishes what was unpacked; packages that were never
                # downloaded (an interrupted install) need apt to fetch them.
                if ! dpkg --force-confdef --force-confold --configure -a >> "$apt_log" 2>&1; then
                    stage "Repairing missing packages"
                    apt-get -o APT::Status-Fd=3 -f install -y 3> "$SUPPORT_DIR/apt-status" >> "$apt_log" 2>&1 || { classify_failure "$label"; return 1; }
                fi
            fi
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
        pacman_install() { # $1: extra -S flags ("" or "yu")
            : > "$status_file"
            { pacman -S$1 --needed --noconfirm --noprogressbar $arch 2>&1; echo "PACMAN_EXIT:$?"; } | tee "$apt_log" | awk -v status="$status_file" '
                /^Packages \([0-9]+\)/ { gsub(/[^0-9]/, "", $2); total = $2 + 0 }
                / downloading\.\.\.$/ { d++; if (total) { printf "dlstatus:%d:%.1f:%s\n", d, 100 * d / total, $1 >> status; fflush(status) } }
                /^\( *[0-9]+\/[0-9]+\) installing / { split($0, a, /[()\/]/); n = a[2] + 0; t = a[3] + 0; name = $NF
                  if (t) { printf "pmstatus:%s:%.1f:Installing %s\n", name, 100 * n / t, name >> status; fflush(status) } }'
            grep -q '^PACMAN_EXIT:0$' "$apt_log"
        }
        # First only what is missing, from the package database already on the
        # device: consistent with the installed system (nothing was refreshed),
        # and no system upgrade. Only if the mirror no longer has those versions,
        # refresh and upgrade in the SAME transaction as the requested packages
        # (-Sy followed by -S would leave rolling Arch partially upgraded).
        if [ -z "${pacman_synced:-}" ] && ls /var/lib/pacman/sync/*.db >/dev/null 2>&1; then
            stage "Installing $label"
            if pacman_install ""; then cat "$apt_log"; return 0; fi
            if grep -qE 'failed retrieving file|404|target not found|could not satisfy dependencies' "$apt_log"; then
                echo "PROFILE: $label: the local package database is older than the mirror; synchronizing"
            else cat "$apt_log"; classify_failure "$label"; return 1; fi
        fi
        pacman_refresh=
        if [ -z "${pacman_synced:-}" ]; then pacman_refresh=yu; fi
        stage "Installing $label${pacman_refresh:+ (synchronizing and upgrading Arch)}"
        pacman_install "$pacman_refresh" || { cat "$apt_log"; classify_failure "$label"; return 1; }
        pacman_synced=1
        cat "$apt_log"
    elif have dnf; then
        [ -n "$fedora" ] || { echo "PROFILE_FAILURE: category=PACKAGE_NOT_FOUND stage=$label detail=no Fedora package adapter for this component"; return 1; }
        stage "Installing $label"
        # Use the configured Fedora repositories and normal RPM signature
        # policy. Do not erase conflicting packages or disable GPG checks.
        run_progress dnf dnf -y --setopt=install_weak_deps=False install $fedora || { cat "$apt_log"; classify_failure "$label"; return 1; }
        cat "$apt_log"
    elif have apk; then
        stage "Installing $label"
        run_progress apk apk add --no-cache $alpine || { cat "$apt_log"; classify_failure "$label"; return 1; }
        cat "$apt_log"
    elif have xbps-install; then
        [ -n "$void" ] || { echo "PROFILE_FAILURE: category=PACKAGE_NOT_FOUND stage=$label detail=no Void Linux package adapter for this component"; return 1; }
        stage "Installing $label"
        # Void's signed repositories (xbps verifies RSA signatures); -S syncs
        # the index in the same transaction as the install.
        run_progress xbps xbps-install -Sy $void || { cat "$apt_log"; classify_failure "$label"; return 1; }
        cat "$apt_log"
    elif have zypper; then
        [ -n "$suse" ] || { echo "PROFILE_FAILURE: category=PACKAGE_NOT_FOUND stage=$label detail=no openSUSE package adapter for this component"; return 1; }
        stage "Installing $label"
        # openSUSE's signed repositories with normal RPM signature policy.
        run_progress zypper zypper --non-interactive install --no-recommends $suse || { cat "$apt_log"; classify_failure "$label"; return 1; }
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

# Pop!_OS / Zorin OS (recorded at first start by the app): layer the vendor's
# signed repository and defaults onto the Ubuntu base once. Runs at the first
# account setup (right after the first launch) and again at desktop setup if
# that did not complete.
apply_flavor() {
    lr_flavor=$(cat /var/lib/linuxrunner/flavor 2>/dev/null || true)
    case "$lr_flavor" in
        pop|zorin)
            if [ ! -f "/var/lib/linuxrunner/$lr_flavor-initialized" ]; then
                case "$lr_flavor" in pop) stage "Setting up Pop!_OS (System76's repository and defaults)";; zorin) stage "Setting up Zorin OS (Zorin's repositories and defaults)";; esac
                printf '#include "%s";\nAPT::Status-Fd "3";\n' "$SUPPORT_DIR/apt-unattended.conf" > "$SUPPORT_DIR/apt-progress.conf"
                APT_CONFIG="$SUPPORT_DIR/apt-progress.conf" /bin/sh "./initialize-$lr_flavor.sh" 3>> "$SUPPORT_DIR/apt-status" || support_fail PROFILE "$lr_flavor-initialization"
            fi;;
    esac
}

repair() {
    want_accounts=${1:-}
    verify_resources
    [ "$(id -u)" = 0 ] || support_fail PROFILE root-required
    apply_flavor
    if gui_missing; then
        install_packages "X server and display support" busybox procps xvfb x11vnc xauth x11-xserver-utils x11-utils x11-apps libx11-dev libxcb1-dev build-essential pkg-config binutils xterm xfonts-base fonts-dejavu-core fontconfig dbus-daemon dbus-x11 librsvg2-common \
            -- xvfb x11vnc xclock libx11-dev libxcb-dev build-base pkgconf binutils xterm font-misc-misc font-dejavu fontconfig xauth setxkbmap xmodmap dbus \
            -- procps-ng xorg-server-xvfb x11vnc xorg-xauth xorg-xset xorg-xrandr xorg-xprop xorg-xwininfo xorg-xdpyinfo xorg-xsetroot xorg-xmodmap xorg-setxkbmap xorg-xclock libx11 libxcb gcc make pkgconf binutils xterm xorg-fonts-misc ttf-dejavu fontconfig dbus librsvg \
            -- procps-ng util-linux-core gawk findutils xorg-x11-server-Xvfb x11vnc xorg-x11-xauth xset xrandr xprop xwininfo xdpyinfo xsetroot xmodmap setxkbmap xclock libX11-devel libxcb-devel libXext-devel gcc make pkgconf-pkg-config binutils xterm xorg-x11-fonts-misc dejavu-sans-fonts fontconfig dbus-daemon dbus-tools dbus-x11 librsvg2 \
            -- procps-ng gawk findutils xorg-server-xvfb x11vnc xauth xset xrandr xprop xwininfo xdpyinfo xsetroot xmodmap setxkbmap xclock libX11-devel libxcb-devel libXext-devel gcc make pkg-config binutils xterm font-misc-misc dejavu-fonts-ttf fontconfig dbus librsvg || support_fail PROFILE gui-packages
        mkdir -p /var/lib/dbus; dbus-uuidgen --ensure 2>/dev/null || true
    fi
    # GPU display server: the distribution's own Xorg (modesetting/glamor) and
    # Mesa drive LinuxRunner's virtual GPU (virgl -> ANGLE -> Metal); see
    # xorg-native.sh. Optional: without it the software display is used.
    if ! { [ -x /usr/lib/xorg/Xorg ] || [ -x /usr/lib/Xorg ] || [ -x /usr/libexec/Xorg ]; } || ! have glxinfo; then
        # Optional but it decides GPU versus software rendering, so a failed
        # download (large Mesa/LLVM packages on a slow link) is retried once.
        gpu_try=1
        while ! install_packages "GPU display server" xserver-xorg-core libgl1-mesa-dri libegl-mesa0 mesa-utils \
            -- xorg-server mesa-dri-gallium mesa-demos \
            -- xorg-server mesa mesa-utils \
            -- xorg-x11-server-Xorg mesa-dri-drivers glx-utils \
            -- xorg-server mesa-dri glxinfo; do
            if [ "$gpu_try" -ge 2 ]; then echo 'PROFILE: GPU display server not installed; the software display remains available'; break; fi
            gpu_try=$((gpu_try + 1)); echo 'PROFILE: GPU display server download failed; retrying once'; sleep 5
        done
    fi
    if gui_missing || gui_helpers_stale; then build_gui_helpers || support_fail PROFILE gui-helpers; fi
    # File managers recognise launchers and file types through the shared MIME
    # database. A package hook that failed (seen once in an interrupted Arch
    # upgrade) leaves it missing: every .desktop launcher then shows as a plain
    # document. Regenerate it when absent; normally a no-op.
    if [ -d /usr/share/mime/packages ] && [ ! -s /usr/share/mime/mime.cache ] && have update-mime-database; then
        stage "Rebuilding the file type database"
        update-mime-database /usr/share/mime || echo 'PROFILE: file type database could not be rebuilt; launchers may show as documents'
    fi
    if desktop_missing; then
        if ! desktop_commands_present; then
            install_packages "desktop (window manager, panel, file manager)" openbox lxpanel pcmanfm adwaita-icon-theme fonts-dejavu-core dbus-daemon dbus-bin xterm \
                -- openbox xfce4-panel pcmanfm adwaita-icon-theme gdk-pixbuf-loaders font-dejavu dbus xterm \
                -- openbox lxqt-panel pcmanfm-qt adwaita-icon-theme ttf-dejavu dbus xterm \
                -- openbox lxpanel lxmenu-data pcmanfm adwaita-icon-theme dejavu-sans-fonts dbus-daemon dbus-tools xterm \
                -- openbox lxpanel pcmanfm adwaita-icon-theme dejavu-fonts-ttf dbus xterm || support_fail PROFILE desktop-packages
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
            if [ "$distro" = fedora ]; then
                # File-capability transactions are now supported. The existing
                # PulseAudio-to-iOS relay still needs separate Fedora audio
                # qualification; do not label an untested sound path ready.
                echo 'PROFILE: Fedora sound relay is not yet qualified; desktop sound remains optional'
            else
                install_packages "sound" pulseaudio pulseaudio-utils libasound2-plugins -- pulseaudio pulseaudio-utils alsa-plugins-pulse -- pulseaudio libpulse pulseaudio-alsa || echo 'PROFILE: sound packages unavailable; desktop continues without sound'
            fi
        fi
        if have parec; then stage "Preparing sound"; build_audio_relay || echo 'PROFILE: sound relay build failed; desktop continues without sound'; fi
    fi
    # Account tools: LinuxRunner's Debian image does not include passwd.
    if [ "$want_accounts" = accounts ] && accounts_missing; then
        install_packages "account tools" passwd -- shadow -- shadow -- shadow-utils -- shadow || support_fail PROFILE account-tools
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
        apply_flavor
        stage "Checking existing installation"
        if accounts_missing; then install_packages "account tools" passwd -- shadow -- shadow -- shadow-utils -- shadow || support_fail PROFILE account-tools; else echo 'PROFILE: account tools already installed'; fi
        if [ "${2:-}" = sudo ] && ! have sudo; then install_packages "administrator tools" sudo -- sudo -- sudo -- sudo -- sudo || support_fail PROFILE sudo; fi
        # Present is not enough: each tool must run.
        stage "Verifying installation"
        accounts_missing && support_fail PROFILE account-tools-missing
        for tool in useradd chpasswd passwd; do "$tool" --help > /dev/null 2>&1 || { echo "PROFILE_FAILURE: category=ACCOUNT_TOOL_BROKEN stage=verify detail=$tool --help failed"; support_fail PROFILE account-tool-broken; }; done
        if [ "${2:-}" = sudo ]; then sudo -V > /dev/null 2>&1 || { echo 'PROFILE_FAILURE: category=ACCOUNT_TOOL_BROKEN stage=verify detail=sudo -V failed'; support_fail PROFILE sudo-broken; }; fi
        echo 'PROFILE: accounts=PASS';;
    plasma)
        # Debian retains its X11 path; current Arch/Fedora use managed KWin
        # Wayland bridge. The regular desktop stays installed as a fallback.
        case "$distro" in
            debian) ;;
            arch|archarm|fedora)
                if have startplasma-x11 && have kwin_x11 && have plasmashell; then
                    repair "${2:-}"; echo 'PROFILE: plasma-packages=present session=not-tested'; exit 0
                fi
                repair "${2:-}"
                missing=0
                for tool in kwin_wayland plasmashell kded6 ksmserver konsole dolphin kate systemsettings pipewire wireplumber dbus-run-session gdbus kscreen-doctor xdg-user-dirs-update; do have "$tool" || missing=1; done
                pkg-config --exists 'libei-1.0 >= 1.6' Qt6Widgets Qt6DBus KF6CoreAddons libvncserver || missing=1
                # KWin's overview imports Milou; Fedora recommends it rather
                # than requiring it, so weak-dependency-free installs need it.
                if [ "$distro" = fedora ] && [ ! -r /usr/lib64/qt6/qml/org/kde/milou/qmldir ]; then missing=1; fi
                if [ "$missing" = 1 ]; then
                    install_packages "KDE Plasma Wayland" -- -- plasma-desktop plasma-workspace kwin konsole dolphin kate systemsettings breeze breeze-icons plasma-integration qt6-base kcoreaddons libei libvncserver krfb pipewire wireplumber xdg-desktop-portal xdg-desktop-portal-kde xdg-user-dirs dbus -- plasma-desktop plasma-workspace kwin konsole dolphin kate plasma-systemsettings plasma-milou breeze-icon-theme plasma-integration qt6-qtbase-devel kf6-kcoreaddons-devel libei-devel libvncserver-devel krfb pipewire wireplumber xdg-desktop-portal xdg-desktop-portal-kde xdg-user-dirs dbus-daemon gcc-c++ || support_fail PROFILE plasma-packages
                fi
                stage "Building the Plasma display bridge"
                /bin/sh ./prepare-plasma-bridge.sh || support_fail PROFILE plasma-bridge-build
                for tool in kwin_wayland plasmashell konsole dolphin kate systemsettings; do have "$tool" || support_fail PROFILE plasma-missing; done
                echo 'PROFILE: plasma-packages=present backend=wayland session=not-tested'; exit 0;;
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
                -- lxqt-session lxqt-panel lxqt-runner lxqt-config pcmanfm-qt qterminal openbox lxqt-themes \
                -- lxqt-session lxqt-panel lxqt-runner lxqt-config pcmanfm-qt qterminal openbox lxqt-themes || support_fail PROFILE lxqt-packages
        fi
        have startlxqt && have lxqt-panel && have pcmanfm-qt || support_fail PROFILE lxqt-missing
        # LXQt's configuration names the Breeze icons, which Debian only
        # recommends: without them the menu, panel and file manager have no icons.
        # Also repairs an LXQt installed before this check existed.
        [ -d /usr/share/icons/breeze-dark ] || install_packages "Breeze icons" breeze-icon-theme -- breeze-icons -- breeze-icons -- breeze-icon-theme || support_fail PROFILE lxqt-icons
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
    gnome)
        [ "$distro_id" = ubuntu ] || support_fail PROFILE gnome-ubuntu-only
        repair "${2:-}"
        if [ "$os_id" = zorin ]; then
            # Zorin OS's own desktop: GNOME Shell in Zorin's session mode with
            # the Zorin menu, taskbar, desktop icons, tiling and themes, from
            # Zorin's signed repository (architecture-independent packages).
            zorin_desktop_present() {
                for package in zorin-desktop-session gnome-shell-extensions-zorin-desktop zorin-desktop-themes zorin-icon-themes zorin-os-default-settings gnome-menus nautilus gnome-terminal dbus-x11; do
                    [ "$(dpkg-query -W -f='${db:Status-Status}' "$package" 2>/dev/null)" = installed ] || return 1
                done
            }
            if ! zorin_desktop_present; then
                install_packages "Zorin Desktop" zorin-desktop-session gnome-shell-extensions-zorin-desktop zorin-desktop-themes zorin-icon-themes zorin-os-default-settings zorin-appearance gnome-menus nautilus gnome-terminal dbus-x11 -- -- -- || support_fail PROFILE gnome-packages
            fi
            # Zorin's default wallpaper (/usr/share/backgrounds/Zorin.jpg).
            # Optional: without it the desktop shows a plain background.
            if [ ! -e /usr/share/backgrounds/Zorin.jpg ]; then
                install_packages "Zorin wallpapers" zorin-os-wallpapers -- -- -- || echo 'PROFILE: Zorin wallpapers unavailable; plain background'
            fi
            # Zorin's wallpapers are 6144x3340: one 82 MB texture, larger than
            # a single GPU buffer may be (64 MB), so GNOME Shell cannot create
            # its background. Install 2560-pixel copies of the same pictures
            # and point Zorin's background setting at them (a higher-priority
            # schema override; Zorin's own files are unchanged).
            if [ -e /usr/share/backgrounds/Zorin.jpg ] && [ ! -e /usr/share/backgrounds/linuxrunner/Zorin-2560.jpg ] && have python3; then
                mkdir -p /usr/share/backgrounds/linuxrunner
                python3 - <<'PYSCALE' || echo 'PROFILE: Zorin wallpaper could not be scaled; plain background'
import gi
gi.require_version("GdkPixbuf", "2.0")
from gi.repository import GdkPixbuf
for name in ("Zorin", "Zorin-Dark"):
    source = "/usr/share/backgrounds/%s.jpg" % name
    try:
        picture = GdkPixbuf.Pixbuf.new_from_file_at_scale(source, 2560, -1, True)
    except Exception:
        continue
    picture.savev("/usr/share/backgrounds/linuxrunner/%s-2560.jpg" % name, "jpeg", ["quality"], ["90"])
PYSCALE
            fi
            if [ -e /usr/share/backgrounds/linuxrunner/Zorin-2560.jpg ]; then
                dark=/usr/share/backgrounds/linuxrunner/Zorin-Dark-2560.jpg; [ -e "$dark" ] || dark=/usr/share/backgrounds/linuxrunner/Zorin-2560.jpg
                printf '[org.gnome.desktop.background]\npicture-uri = %s\npicture-uri-dark = %s\n\n[org.gnome.desktop.screensaver]\npicture-uri = %s\n' \
                    "'file:///usr/share/backgrounds/linuxrunner/Zorin-2560.jpg'" "'file://$dark'" "'file:///usr/share/backgrounds/linuxrunner/Zorin-2560.jpg'" \
                    > /usr/share/glib-2.0/schemas/90_linuxrunner-zorin.gschema.override
                glib-compile-schemas /usr/share/glib-2.0/schemas || echo 'PROFILE: schema compile failed'
            fi
            # zorin-desktop-themes links its GNOME Shell themes into
            # /usr/share/gnome-shell/theme only if GNOME Shell was already
            # installed when it was configured. Zorin OS setup installs the
            # themes first, so run the package's own configure step again.
            zorin_shell_theme=$(sed -n 's/.*"themeResourceName": *"theme\/\([^/]*\)\/.*/\1/p' /usr/share/gnome-shell/modes/zorin.json 2>/dev/null)
            if [ -n "$zorin_shell_theme" ] && [ ! -e "/usr/share/gnome-shell/theme/$zorin_shell_theme/gnome-shell-theme.gresource" ] &&
                [ -x /var/lib/dpkg/info/zorin-desktop-themes.postinst ]; then
                stage "Linking Zorin GNOME Shell themes"
                /var/lib/dpkg/info/zorin-desktop-themes.postinst configure || echo 'PROFILE: zorin-desktop-themes configure step failed'
            fi
            [ -z "$zorin_shell_theme" ] || [ -e "/usr/share/gnome-shell/theme/$zorin_shell_theme/gnome-shell-theme.gresource" ] || support_fail PROFILE zorin-shell-theme-missing
            zorin_desktop_present && have gnome-shell && [ -x /usr/lib/systemd/systemd-logind ] && [ -f /usr/share/gnome-shell/modes/zorin.json ] && [ -d /usr/share/gnome-shell/extensions/zorin-taskbar@zorinos.com ] || support_fail PROFILE gnome-missing
            echo 'PROFILE: gnome-packages=present flavor=zorin session=not-tested'
            exit 0
        fi
        if ! gnome_packages_present; then
            install_packages "Ubuntu GNOME" gnome-shell gnome-session ubuntu-session gnome-shell-extension-ubuntu-dock ubuntu-settings ubuntu-wallpapers yaru-theme-gnome-shell yaru-theme-gtk yaru-theme-icon nautilus gnome-terminal dbus-x11 -- -- -- || support_fail PROFILE gnome-packages
        fi
        gnome_packages_present && have gnome-shell && have gnome-session && [ -x /usr/lib/systemd/systemd-logind ] && [ -f /usr/share/gnome-shell/modes/ubuntu.json ] && [ -d /usr/share/gnome-shell/extensions/ubuntu-dock@ubuntu.com ] || support_fail PROFILE gnome-missing
        echo 'PROFILE: gnome-packages=present session=not-tested';;
    mate)
        # MATE from the distribution's own packages (core session, no extras).
        repair "${2:-}"
        if ! { have mate-session && have marco && have mate-panel; }; then
            install_packages "MATE" mate-desktop-environment-core mate-terminal \
                -- mate-desktop mate-session-manager marco mate-panel caja mate-terminal mate-settings-daemon \
                -- mate mate-terminal \
                -- mate-session-manager marco mate-panel caja mate-terminal mate-settings-daemon || support_fail PROFILE mate-packages
        fi
        have mate-session && have marco && have mate-panel || support_fail PROFILE mate-missing
        # The distribution's schema override picks the panel layout (Ubuntu:
        # 'familiar'). Every applet that layout names must exist, or MATE
        # opens one "Error loading applet" dialog per missing factory.
        # Install the distribution packages that provide them (apt only).
        if command -v apt-get >/dev/null 2>&1; then
            mate_layout=$(sed -n "s/^default-layout=['\"]*\([^'\"]*\).*/\1/p" /usr/share/glib-2.0/schemas/*.gschema.override 2>/dev/null | tail -n 1)
            mate_layout_file=/usr/share/mate-panel/layouts/${mate_layout:-default}.layout
            mate_missing=
            if [ -f "$mate_layout_file" ]; then
                for factory in $(sed -n 's/^applet-iid=\([^:]*\)::.*/\1/p' "$mate_layout_file" | sort -u); do
                    grep -qs "^Id=$factory\$" /usr/share/mate-panel/applets/*.mate-panel-applet && continue
                    case "$factory" in
                        BriskMenuFactory) mate_missing="$mate_missing mate-applet-brisk-menu";;
                        IndicatorAppletCompleteFactory|IndicatorAppletFactory) mate_missing="$mate_missing mate-indicator-applet";;
                        TrashAppletFactory|CommandAppletFactory|NetspeedAppletFactory|MultiLoadAppletFactory|StickyNotesAppletFactory) mate_missing="$mate_missing mate-applets";;
                        *) echo "PROFILE: mate layout applet $factory has no known package";;
                    esac
                done
            fi
            if [ -n "$mate_missing" ]; then
                # shellcheck disable=SC2086
                install_packages "MATE panel applets" $(printf '%s\n' $mate_missing | sort -u) -- -- -- ||
                    echo 'PROFILE: mate panel applets could not be installed; the panel may report missing applets'
            fi
            echo "PROFILE: mate layout=${mate_layout:-default}"
        fi
        echo 'PROFILE: mate=PASS';;
    cinnamon)
        # Cinnamon; not packaged by Alpine.
        repair "${2:-}"
        [ "$distro" != alpine ] || support_fail PROFILE cinnamon-not-packaged
        if ! { have cinnamon-session && have cinnamon; }; then
            install_packages "Cinnamon" cinnamon-core \
                -- \
                -- cinnamon gnome-terminal \
                -- cinnamon cinnamon-session gnome-terminal || support_fail PROFILE cinnamon-packages
        fi
        have cinnamon-session && have cinnamon || support_fail PROFILE cinnamon-missing
        echo 'PROFILE: cinnamon=PASS';;
    cosmic)
        # Pop!_OS's own desktop (COSMIC) from System76's repository.
        repair "${2:-}"
        [ "$os_id" = pop ] || support_fail PROFILE cosmic-needs-pop
        if ! { have cosmic-session && have cosmic-comp && have cosmic-panel; }; then
            install_packages "COSMIC Desktop" cosmic-session cosmic-term cosmic-files cosmic-wallpapers cosmic-icons -- -- -- || support_fail PROFILE cosmic-packages
        fi
        have cosmic-session && have cosmic-comp && have cosmic-panel || support_fail PROFILE cosmic-missing
        # window-fit keeps cosmic-comp's nested output at the screen size.
        if [ ! -x window-fit ] || [ window-fit.c -nt window-fit ]; then
            gcc window-fit.c -O2 -Wall -lX11 -o window-fit.new && chmod 755 window-fit.new && mv window-fit.new window-fit || support_fail PROFILE window-fit
        fi
        echo 'PROFILE: cosmic=PASS';;
    budgie)
        # Budgie; not packaged by Alpine.
        repair "${2:-}"
        [ "$distro" != alpine ] || support_fail PROFILE budgie-not-packaged
        if ! { have budgie-desktop && have budgie-panel; }; then
            install_packages "Budgie" budgie-desktop gnome-terminal \
                -- \
                -- budgie gnome-terminal \
                -- budgie-desktop gnome-terminal || support_fail PROFILE budgie-packages
        fi
        have budgie-desktop && have budgie-panel || support_fail PROFILE budgie-missing
        echo 'PROFILE: budgie=PASS';;
    i3)
        repair "${2:-}"
        if ! { have i3 && have xterm && have dmenu_run; }; then
            install_packages "i3" i3-wm suckless-tools xterm \
                -- i3wm dmenu xterm \
                -- i3-wm dmenu xterm \
                -- i3 dmenu xterm || support_fail PROFILE i3-packages
        fi
        have i3 && have xterm && have dmenu_run || support_fail PROFILE i3-missing
        echo 'PROFILE: i3=PASS';;
    wayfire)
        # Wayfire (wlroots), its wf-shell panel/dock/background, the foot terminal
        # and the WCM settings app from the distribution's packages. It runs
        # inside the session's X server with wlroots' software renderer.
        repair "${2:-}"
        [ "$distro" != alpine ] || support_fail PROFILE wayfire-unavailable-on-alpine
        if ! { have wayfire && have wf-panel && have foot; }; then
            install_packages "Wayfire" wayfire wf-shell foot wcm xwayland \
                -- wayfire \
                -- wayfire wf-shell foot wcm xorg-xwayland \
                -- wayfire wf-shell foot xorg-x11-server-Xwayland seatd wayland-devel wayland-protocols-devel libxkbcommon-devel || support_fail PROFILE wayfire-packages
        fi
        have wayfire && have wf-panel && have foot || support_fail PROFILE wayfire-missing
        # The GPU session uses seatd and builds the shared KMS bridge. Older
        # Wayfire profiles installed only the desktop commands, which silently
        # forced software rendering when seatd or development headers were absent.
        if ! { have seatd && have gcc && have pkg-config && pkg-config --exists wayland-client xkbcommon; }; then
            install_packages "Wayfire display dependencies" seatd gcc pkg-config libwayland-dev libxkbcommon-dev \
                -- -- seatd gcc pkgconf wayland libxkbcommon \
                -- seatd gcc pkgconf-pkg-config wayland-devel libxkbcommon-devel || support_fail PROFILE wayfire-display-dependencies
        fi
        have seatd && have gcc && have pkg-config && pkg-config --exists wayland-client xkbcommon ||
            support_fail PROFILE wayfire-display-dependencies-missing
        echo 'PROFILE: wayfire=PASS';;
    xfce)
        # Xfce 4 X11 session from the distribution's packages, on top of the
        # regular desktop (which stays the fallback). On Kali this is Kali's
        # own desktop: kali-desktop-xfce (Kali menu, themes and wallpaper).
        repair "${2:-}"
        if ! { have startxfce4 && have xfce4-panel && have xfwm4 && have xfdesktop; }; then
            case "$distro_id" in
                kali) install_packages "Kali desktop (Xfce)" kali-desktop-xfce -- -- -- || support_fail PROFILE xfce-packages;;
                # Debian and Ubuntu-based: the distribution's Xfce desktop
                # (session, panel, desktop, Thunar, terminal, settings).
                debian|ubuntu|pop|zorin) install_packages "Xfce" xfce4 xfce4-terminal xfce4-notifyd dbus-x11 -- -- -- || support_fail PROFILE xfce-packages;;
                # Void Linux's own desktop: its official installer images ship Xfce.
                void) install_packages "Xfce" -- -- -- -- xfce4 xfce4-terminal xfce4-notifyd dbus-x11 || support_fail PROFILE xfce-packages;;
                *) support_fail PROFILE xfce-not-yet-qualified-on-$distro_id;;
            esac
        fi
        have startxfce4 && have xfce4-panel && have xfwm4 && have xfdesktop || support_fail PROFILE xfce-missing
        echo 'PROFILE: xfce=PASS';;
    kali-tools)
        # Kali's standard tool selection (kali-linux-default), from Kali's
        # signed archive. Large: about 1.5 GB download, 6 GB installed.
        [ "$distro_id" = kali ] || support_fail PROFILE kali-tools-need-kali
        [ "$(id -u)" = 0 ] || support_fail PROFILE root-required
        if ! dpkg-query -W -f='${Status}' kali-linux-default 2>/dev/null | grep -q 'install ok installed'; then
            install_packages "Kali standard tools" kali-linux-default -- -- -- || support_fail PROFILE kali-tools-packages
        fi
        dpkg-query -W -f='${Status}' kali-linux-default 2>/dev/null | grep -q 'install ok installed' || support_fail PROFILE kali-tools-missing
        echo 'PROFILE: kali-tools=PASS';;
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
    mode-agent)
        # Gemini CLI is not offered: it needs WebAssembly, which Node lacks in
        # LinuxRunner's interpreter (--jitless).
        # Agent Mode (opt-in): command-line AI coding agents with the tools they
        # use. Each agent is its own npm package; one failing does not stop the
        # others. Sign-in happens inside each agent (no keys are stored here).
        [ "$(id -u)" = 0 ] || support_fail PROFILE root-required
        apply_flavor
        stage "Installing developer tools"
        if ! { have node && have npm && have git && have rg; }; then
            install_packages "developer tools" nodejs npm git curl ca-certificates ripgrep python3 build-essential \
                -- nodejs npm git curl ca-certificates ripgrep python3 build-base bash \
                -- nodejs npm git curl ca-certificates ripgrep python base-devel \
                -- nodejs npm git curl ca-certificates ripgrep python3 gcc make \
                -- nodejs git curl ca-certificates ripgrep python3 base-devel || support_fail PROFILE agent-tools
        fi
        have node && have npm || support_fail PROFILE node-missing
        node_major=$(node -p 'process.versions.node.split(".")[0]' 2>/dev/null || echo 0)
        echo "PROFILE: node=$(node --version 2>/dev/null) npm=$(npm --version 2>/dev/null)"
        agents_ok=
        for spec in "Claude Code:@anthropic-ai/claude-code:claude:18" "Codex CLI:@openai/codex:codex:18" "OpenCode:opencode-ai:opencode:18"; do
            title=${spec%%:*}; rest=${spec#*:}; package=${rest%%:*}; rest=${rest#*:}; command=${rest%%:*}; need=${rest#*:}
            stage "Installing $title"
            if have "$command"; then echo "PROFILE: agent $command already installed"; agents_ok="$agents_ok $command"; continue; fi
            if [ "$node_major" -lt "$need" ]; then echo "PROFILE: agent $command skipped: needs Node $need+, this distribution has $node_major"; continue; fi
            if npm install -g --no-fund --no-audit --loglevel=error "$package"; then
                have "$command" && agents_ok="$agents_ok $command" && echo "PROFILE: agent $command=PASS"
            else
                echo "PROFILE: agent $command install failed; the other agents are kept"
            fi
        done
        [ -n "$agents_ok" ] || support_fail PROFILE agents-missing
        install -m 755 ./lr-agent.sh /usr/local/bin/lr-agent || support_fail PROFILE agent-launcher
        echo "PROFILE: agents=$agents_ok"
        echo 'PROFILE: mode-agent=PASS';;
    mode-rdp)
        # RDP Mode (opt-in): xrdp shares the running LinuxRunner desktop with
        # Remote Desktop clients on the same network (modes.sh starts it).
        [ "$(id -u)" = 0 ] || support_fail PROFILE root-required
        apply_flavor
        stage "Installing Remote Desktop server"
        if ! have xrdp; then
            install_packages "xrdp" xrdp -- xrdp -- xrdp -- xrdp -- xrdp || support_fail PROFILE xrdp-packages
        fi
        have xrdp || support_fail PROFILE xrdp-missing
        echo 'PROFILE: mode-rdp=PASS';;
    mode-ssh)
        # SSH Mode (opt-in): OpenSSH server on port 2222 for terminals, editors
        # (VS Code Remote) and file transfer from a computer.
        [ "$(id -u)" = 0 ] || support_fail PROFILE root-required
        apply_flavor
        stage "Installing SSH server"
        if [ ! -x /usr/sbin/sshd ]; then
            install_packages "OpenSSH server" openssh-server -- openssh-server -- openssh -- openssh-server -- openssh || support_fail PROFILE ssh-packages
        fi
        [ -x /usr/sbin/sshd ] || support_fail PROFILE sshd-missing
        echo 'PROFILE: mode-ssh=PASS';;
    mode-liftoff)
        # Liftoff (Dev Server mode, opt-in): Python, Node.js and git; modes.sh
        # serves ~/Sites on port 8080 and any dev server can listen on 0.0.0.0.
        [ "$(id -u)" = 0 ] || support_fail PROFILE root-required
        apply_flavor
        stage "Installing web development tools"
        if ! { have python3 && have node && have npm && have git; }; then
            install_packages "web development tools" python3 nodejs npm git curl ca-certificates \
                -- python3 nodejs npm git curl ca-certificates \
                -- python nodejs npm git curl ca-certificates \
                -- python3 nodejs npm git curl ca-certificates \
                -- python3 nodejs git curl ca-certificates || support_fail PROFILE liftoff-tools
        fi
        have python3 && have node || support_fail PROFILE liftoff-missing
        mkdir -p /var/lib/linuxrunner && touch /var/lib/linuxrunner/mode-liftoff
        echo 'PROFILE: mode-liftoff=PASS';;
    mode-pocket)
        # Pocket Cloud (Pocket Server mode, opt-in): WsgiDAV (WebDAV + web file
        # browser, pure Python) in its own virtual environment.
        [ "$(id -u)" = 0 ] || support_fail PROFILE root-required
        apply_flavor
        stage "Installing Python"
        # venv imports without Debian's python3-venv, but cannot create an
        # environment (no ensurepip); check what creating one needs.
        if ! { have python3 && python3 -c 'import ensurepip, venv' >/dev/null 2>&1; }; then
            install_packages "Python" python3 python3-venv python3-pip -- python3 py3-pip -- python python-pip -- python3 python3-pip -- python3 python3-pip || support_fail PROFILE pocket-python
        fi
        stage "Installing Pocket Cloud server"
        venv=/opt/linuxrunner/pocket-cloud
        if [ ! -x "$venv/bin/wsgidav" ]; then
            rm -rf "$venv"   # a failed earlier attempt leaves a broken environment
            python3 -m venv "$venv" || support_fail PROFILE pocket-venv
            "$venv/bin/pip" install --disable-pip-version-check --no-cache-dir -q wsgidav cheroot || support_fail PROFILE pocket-server
        fi
        [ -x "$venv/bin/wsgidav" ] || support_fail PROFILE pocket-missing
        touch /var/lib/linuxrunner/mode-pocket 2>/dev/null || { mkdir -p /var/lib/linuxrunner && touch /var/lib/linuxrunner/mode-pocket; }
        echo 'PROFILE: mode-pocket=PASS';;
    mode-arcadia)
        # Arcadia (Retro Lab mode, opt-in): RetroArch and the classic-console
        # cores the distribution packages. No games are included.
        [ "$(id -u)" = 0 ] || support_fail PROFILE root-required
        apply_flavor
        stage "Installing RetroArch"
        if ! have retroarch; then
            install_packages "RetroArch" retroarch -- retroarch -- retroarch -- retroarch -- retroarch || support_fail PROFILE arcadia-retroarch
        fi
        have retroarch || support_fail PROFILE arcadia-missing
        stage "Installing emulator cores"
        if have apt-get; then cores="libretro-nestopia libretro-snes9x libretro-gambatte libretro-mgba libretro-genesisplusgx libretro-beetle-pce-fast"
        elif have pacman; then cores="libretro-nestopia libretro-snes9x libretro-gambatte libretro-mgba libretro-genesis-plus-gx libretro-beetle-pce"
        elif have xbps-install; then cores="libretro-nestopia libretro-snes9x libretro-gambatte libretro-mgba libretro-genesis-plus-gx"
        else cores=; fi
        installed_cores=
        for core in $cores; do
            if install_packages "$core" "$core" -- "$core" -- "$core" -- "$core" -- "$core" >/dev/null 2>&1; then installed_cores="$installed_cores $core"; else echo "PROFILE: core $core not offered here"; fi
        done
        echo "PROFILE: cores=${installed_cores# }"
        mkdir -p /var/lib/linuxrunner && touch /var/lib/linuxrunner/mode-arcadia
        echo 'PROFILE: mode-arcadia=PASS';;
    *) exit 2;;
esac
