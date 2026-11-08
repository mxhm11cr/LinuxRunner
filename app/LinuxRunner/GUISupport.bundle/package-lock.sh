#!/bin/sh
# Shared by the managed support installers and App Manager, per environment.
# Every LinuxRunner package operation (installer-run.sh, package-manager.sh)
# takes this lock, then waits until no other package operation runs in Linux:
# on Debian the first-start configuration (init.sh → initialize-debian.sh →
# dpkg --configure -a) must finish first. Until it does dpkg holds its lock and
# ca-certificates has not built the trust store, so apt fails with "could not
# get lock" and "certificate verify failed" (IPA 890). Locks are never removed
# and package processes are never killed here.
package_lock_acquire() {
    # One lock in the guest rootfs, independent of the support deployment path.
    # Keep fd 9 inherited by workers: a terminated UI/shell must not release
    # ownership while its package manager is still running. Never unlink it.
    mkdir -p /var/lib/linuxrunner || return 1
    if command -v flock >/dev/null 2>&1; then
        [ ! -L /var/lib/linuxrunner/package-operation.lock ] || return 75
        exec 9>>/var/lib/linuxrunner/package-operation.lock || return 1
        flock -n 9 || { echo 'PACKAGE_LOCK: category=PACKAGE_MANAGER_BUSY managed operation already running'; return 75; }
        [ "${1:-}" = defer-ready ] || package_system_ready
        return $?
    fi
    # Minimal Alpine images without flock retain the portable directory lock.
    lock=/var/lib/linuxrunner/package-operation.lock.d
    if ! mkdir "$lock" 2>/dev/null; then
        [ ! -L "$lock" ] || { echo 'PACKAGE_LOCK: unsafe symlink'; return 75; }
        owner=$(cat "$lock/pid" 2>/dev/null || true)
        case "$owner" in ''|*[!0-9]*)
            created=$(stat -c %Y "$lock" 2>/dev/null || date +%s); now=$(date +%s)
            [ "$((now-created))" -ge 60 ] || { echo 'PACKAGE_LOCK: owner publication pending'; return 75; }
            owner=0;;
        esac
        if [ "$owner" != 0 ] && kill -0 "$owner" 2>/dev/null; then
            echo "PACKAGE_LOCK: category=PACKAGE_MANAGER_BUSY managed operation already running pid=$owner"; return 75
        fi
        echo 'PACKAGE_LOCK: recovering abandoned LinuxRunner lock (its owner is gone)'
        rm -f "$lock/pid"; rmdir "$lock" || return 75; mkdir "$lock" || return 75
    fi
    echo $$ > "$lock/pid"
    trap 'rm -f "$lock/pid"; rmdir "$lock" 2>/dev/null || true' EXIT
    [ "${1:-}" = defer-ready ] || package_system_ready
}

# Other package processes in this Linux environment (pid and command line),
# excluding this process tree's own shell. Matched by the program actually
# running (argv[0], or the script a shell runs), never by text elsewhere on the
# command line, so e.g. "grep initialize-debian.sh" is not a package process.
package_processes() {
    for lr_proc in /proc/[0-9]*; do
        lr_pid=${lr_proc#/proc/}
        [ "$lr_pid" != "$$" ] || continue
        lr_cmd=$( { tr '\000' ' ' < "$lr_proc/cmdline"; } 2>/dev/null) || continue
        lr_arg0=${lr_cmd%% *} lr_rest=${lr_cmd#* }; lr_arg1=${lr_rest%% *}
        case "${lr_arg0##*/}" in
            dpkg|apt-get|apt|apk|aptitude|unattended-upgrade|pacman|pacman-key|dnf|dnf5|rpm|initialize-debian.sh|initialize-arch.sh) echo "$lr_pid $lr_cmd";;
            sh|dash|bash) case "${lr_arg1##*/}" in initialize-debian.sh|initialize-arch.sh|pacman-key) echo "$lr_pid $lr_cmd";; esac;;
            *) case "$lr_arg0" in /usr/lib/apt/methods/*) echo "$lr_pid $lr_cmd";; esac;;
        esac
    done
}

# libalpm keeps the O_EXCL db.lck descriptor open; the file contains no PID.
# Scan descriptors too, so a differently named libalpm client is protected.
# An unreadable live process makes recovery unsafe. Disappearing processes
# are harmless; absence of /proc is not evidence that nobody owns the lock.
pacman_lock_holders() {
    [ -r /proc/1/cmdline ] && [ -d /proc/1/fd ] || return 1
    for lr_proc in /proc/[0-9]*; do
        [ -d "$lr_proc" ] || continue
        [ -r "$lr_proc/cmdline" ] && [ -r "$lr_proc/fd" ] || { [ ! -d "$lr_proc" ] || return 1; continue; }
        for lr_fd in "$lr_proc"/fd/*; do
            lr_target=$(readlink "$lr_fd" 2>/dev/null) || continue
            case "$lr_target" in /var/lib/pacman/db.lck|'/var/lib/pacman/db.lck (deleted)') echo "${lr_proc#/proc/} fd=${lr_fd##*/} $lr_target";; esac
        done
    done
}

pacman_current_state_ready() {
    lr_database_ok=1
    lr_database_detail=$(pacman -Dk 2>&1) || lr_database_ok=0
    lr_gcc_orphan=0
    if [ -e /usr/bin/gcc ] || [ -L /usr/bin/gcc ]; then
        pacman -Q gcc >/dev/null 2>&1 && [ "$(pacman -Qqo /usr/bin/gcc 2>/dev/null)" = gcc ] || lr_gcc_orphan=1
    fi
    if [ -e /var/lib/linuxrunner/pacman-recovery-required ] || [ "$lr_database_ok" = 0 ] || [ "$lr_gcc_orphan" = 1 ]; then
        # Read-only diagnosis remains available; it does not clear any state.
        [ "${LR_PACKAGE_DIAGNOSIS:-0}" = 1 ] && return 0
        lr_recovery_script=${LR_PACMAN_RECOVERY_SCRIPT:-/home/mobile/Downloads/.linuxrunner/gui/pacman-recover.sh}
        [ -f "$lr_recovery_script" ] && [ ! -L "$lr_recovery_script" ] || {
            printf '%s\n' "$lr_database_detail"
            echo 'PACKAGE_LOCK: category=PACKAGE_DATABASE_RECOVERY_REQUIRED signed-package recovery support is missing'; return 1;
        }
        /bin/bash "$lr_recovery_script" || return $?
    fi
    return 0
}

pacman_database_ready() {
    lr_db_lock=/var/lib/pacman/db.lck
    [ -e "$lr_db_lock" ] || [ -L "$lr_db_lock" ] || { pacman_current_state_ready; return $?; }
    [ -f "$lr_db_lock" ] && [ ! -L "$lr_db_lock" ] || {
        echo 'PACKAGE_LOCK: category=PACKAGE_DATABASE_RECOVERY_REQUIRED unsafe pacman lock type'; return 1;
    }
    lr_identity=$(stat -c '%d:%i:%s:%Y:%h' "$lr_db_lock") || return 1
    # Two quiescent observations, under LinuxRunner's rootfs-local lock.
    # Existing db.lck prevents a new pacman transaction during this interval.
    for lr_scan in 1 2; do
        lr_active=$(package_processes)
        lr_holders=$(pacman_lock_holders) || {
            echo 'PACKAGE_LOCK: category=PACKAGE_DATABASE_RECOVERY_REQUIRED cannot inspect pacman lock owners'; return 1;
        }
        if [ -n "$lr_active$lr_holders" ]; then
            echo 'PACKAGE_LOCK: category=PACKAGE_MANAGER_BUSY pacman lock has an active owner'
            printf '%s\n%s\n' "$lr_active" "$lr_holders" | sed 's/^/PACKAGE_DETAIL: /'
            return 75
        fi
        [ "$lr_scan" = 2 ] || sleep 2
    done
    [ "$(stat -c '%d:%i:%s:%Y:%h' "$lr_db_lock" 2>/dev/null)" = "$lr_identity" ] || {
        echo 'PACKAGE_LOCK: category=PACKAGE_MANAGER_BUSY pacman lock changed during inspection'; return 75;
    }
    mkdir -p /var/log || return 1
    lr_recovery_log=/var/log/linuxrunner-package-recovery.log
    [ ! -L "$lr_recovery_log" ] || return 1
    if [ -f "$lr_recovery_log" ] && [ "$(stat -c %s "$lr_recovery_log")" -gt 65536 ]; then
        tail -c 32768 "$lr_recovery_log" > "$lr_recovery_log.new" && mv "$lr_recovery_log.new" "$lr_recovery_log" || return 1
    fi
    printf 'time=%s lock=%s identity=%s processes=none descriptors=none recovery=stale-lock\n' "$(date +%s)" "$lr_db_lock" "$lr_identity" >> "$lr_recovery_log" || return 1
    rm -- "$lr_db_lock" || return 1
    # A stale lock warrants current file validation, not a permanent historical
    # transaction gate. Preserve an existing repair plan if one is present.
    if [ ! -e /var/lib/linuxrunner/pacman-recovery-required ]; then
        [ ! -L /var/lib/linuxrunner/pacman-recovery-required ] || return 1
        printf 'abandoned-lock-needs-validation\n' > /var/lib/linuxrunner/pacman-recovery-required
    fi
    echo 'PACKAGE_STAGE: Removed an abandoned pacman lock; validating the current package state'
    pacman_current_state_ready
}

# Wait for Debian's first-start configuration and for any other package
# operation. Progress: PACKAGE_STAGE lines (shown by the app). Fails with
# category PACKAGE_MANAGER_STUCK only when nothing progresses for 15 minutes.
package_system_ready() {
    if [ "$(id -u)" = 0 ] && [ -f /etc/fedora-release ] &&
        { [ ! -f /var/lib/linuxrunner/initialized ] || ! command -v awk >/dev/null 2>&1; } &&
        [ -r ./initialize-fedora.sh ]; then
        echo 'PACKAGE_STAGE: Validating Fedora first-start package state'
        /bin/sh ./initialize-fedora.sh >> /var/log/linuxrunner-initialize.log 2>&1 || {
            echo 'PACKAGE_LOCK: category=PACKAGE_DATABASE_RECOVERY_REQUIRED Fedora first-start validation failed'
            tail -n 8 /var/log/linuxrunner-initialize.log | sed 's/^/PACKAGE_DETAIL: /'
            return 1
        }
    fi
    if [ "$(id -u)" = 0 ] && grep -qx 'ID=kali' /etc/os-release 2>/dev/null &&
        [ ! -f /var/lib/linuxrunner/initialized ] && [ -r ./initialize-kali.sh ]; then
        echo 'PACKAGE_STAGE: Validating Kali Linux first-start package state'
        /bin/sh ./initialize-kali.sh >> /var/log/linuxrunner-initialize.log 2>&1 || {
            echo 'PACKAGE_LOCK: category=PACKAGE_DATABASE_RECOVERY_REQUIRED Kali first-start validation failed'
            tail -n 8 /var/log/linuxrunner-initialize.log | sed 's/^/PACKAGE_DETAIL: /'
            return 1
        }
    fi
    if [ -x /usr/lib/linuxrunner/initialize-arch.sh ] && [ "$(id -u)" = 0 ] && [ ! -f /var/lib/linuxrunner/initialized ]; then
        echo 'PACKAGE_STAGE: Finishing Arch Linux ARM setup (first start)'
        # The initializer has its own inherited flock, shared with boot init.
        /usr/lib/linuxrunner/initialize-arch.sh >> /var/log/linuxrunner-initialize.log 2>&1 || {
            echo 'PACKAGE_LOCK: category=PACKAGE_DATABASE_RECOVERY_REQUIRED Arch first-start configuration failed'
            tail -n 8 /var/log/linuxrunner-initialize.log | sed 's/^/PACKAGE_DETAIL: /'
            return 1
        }
    fi
    if [ -x /usr/lib/linuxrunner/initialize-debian.sh ] && [ "$(id -u)" = 0 ]; then
        # apt itself waits for dpkg's lock instead of failing (all apt users).
        if [ -d /etc/apt/apt.conf.d ] && [ ! -e /etc/apt/apt.conf.d/90linuxrunner-lock-wait ]; then
            printf 'DPkg::Lock::Timeout "900";\n' > /etc/apt/apt.conf.d/90linuxrunner-lock-wait.new &&
                mv /etc/apt/apt.conf.d/90linuxrunner-lock-wait.new /etc/apt/apt.conf.d/90linuxrunner-lock-wait
        fi
        # Private names: these functions run inside installer-run.sh, whose
        # own $log must not be replaced.
        lr_init_log=/var/log/linuxrunner-initialize.log lr_last= lr_idle=0 lr_named=
        while [ ! -f /var/lib/linuxrunner/initialized ]; do
            # init.sh may be about to start it (app just launched): make sure
            # twice, 5 s apart, that neither it nor dpkg runs before starting it.
            if ! package_processes | grep -q -e 'initialize-debian.sh' -e 'dpkg' && sleep 5 &&
                ! package_processes | grep -q -e 'initialize-debian.sh' -e 'dpkg'; then
                # Not running: interrupted earlier or never started. It resumes
                # where it stopped (dpkg --configure -a, then the trust store).
                echo 'PACKAGE_STAGE: Finishing Debian setup (first start)'
                if ! /usr/lib/linuxrunner/initialize-debian.sh >> "$lr_init_log" 2>&1; then
                    echo 'PACKAGE_LOCK: category=PACKAGE_DATABASE_RECOVERY_REQUIRED Debian first-start configuration failed'
                    tail -n 8 "$lr_init_log" | sed 's/^/PACKAGE_DETAIL: /'
                    return 1
                fi
                continue
            fi
            lr_configured=$(grep -c '^Setting up ' "$lr_init_log" 2>/dev/null || true)
            # Real total: configured so far plus what dpkg still lists as unpacked.
            lr_pending=$(dpkg-query -W -f '${Status}\n' 2>/dev/null | grep -c 'unpacked\|half-configured' || true)
            lr_current=$(grep '^Setting up ' "$lr_init_log" 2>/dev/null | tail -n 1 | cut -d' ' -f3)
            # Name the processes waited on once (the device log then shows
            # which pid holds dpkg's lock).
            [ -n "$lr_named" ] || { lr_named=1; package_processes | sed 's/^/PACKAGE_DETAIL: waiting for /'; }
            if [ "$lr_configured" != "$lr_last" ]; then lr_last=$lr_configured lr_idle=0
                echo "PACKAGE_STAGE: Finishing Debian setup (first start): ${lr_configured:-0} packages configured${lr_current:+, now $lr_current}"
                echo "PACKAGE_PROGRESS: ${lr_configured:-0} $(( ${lr_configured:-0} + ${lr_pending:-0} ))"
            else lr_idle=$((lr_idle+3)); fi
            if [ "$lr_idle" -ge 900 ]; then
                echo "PACKAGE_LOCK: category=PACKAGE_MANAGER_STUCK Debian first-start configuration made no progress for 15 minutes (last: ${lr_current:-none})"
                package_processes | sed 's/^/PACKAGE_DETAIL: running /'
                return 1
            fi
            sleep 3
        done
    fi
    # Any other package process (e.g. a terminal apt-get): wait for it.
    lr_waited=0
    while :; do
        lr_others=$(package_processes)
        if command -v pacman >/dev/null 2>&1 && [ -e /var/lib/pacman/db.lck ]; then
            lr_owners=$(pacman_lock_holders) || {
                echo 'PACKAGE_LOCK: category=PACKAGE_DATABASE_RECOVERY_REQUIRED cannot inspect pacman lock owners'; return 1;
            }
            lr_others="${lr_others}${lr_owners:+
$lr_owners}"
        fi
        [ -n "$lr_others" ] || break
        [ "$lr_waited" -gt 0 ] || { echo 'PACKAGE_STAGE: Waiting for another package installation to finish'; printf '%s\n' "$lr_others" | sed 's/^/PACKAGE_DETAIL: running /'; }
        lr_waited=$((lr_waited+3)); [ "$lr_waited" -lt 1800 ] || { echo 'PACKAGE_LOCK: category=PACKAGE_MANAGER_BUSY another package operation is still running after 30 minutes'; return 75; }
        sleep 3
    done
    if command -v pacman >/dev/null 2>&1; then pacman_database_ready || return $?; fi
    return 0
}
