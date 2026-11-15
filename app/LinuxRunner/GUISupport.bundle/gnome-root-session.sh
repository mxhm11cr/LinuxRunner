#!/bin/sh
# Root-side supervisor for an experimental GNOME desktop. Start real guest
# services only when absent and stop only the children this session owns.
set -eu
session=$1 user=$2 support=$3 mode=$4
bus_pid= bus_inode= logind_pid= user_pid=
owned_live() {
    [ -n "$1" ] && [ -r "/proc/$1/status" ] || return 1
    parent=$(awk '/^PPid:/{print $2}' "/proc/$1/status" 2>/dev/null)
    state=$(awk '/^State:/{print $2}' "/proc/$1/status" 2>/dev/null)
    [ "$parent" = "$$" ] && [ "$state" != Z ]
}
cleanup() {
    trap - EXIT HUP INT TERM
    for child in "$user_pid" "$logind_pid" "$bus_pid"; do
        if owned_live "$child"; then kill "$child" 2>/dev/null || true; fi
    done
    for child in "$user_pid" "$logind_pid" "$bus_pid"; do
        [ -n "$child" ] || continue
        i=0
        while owned_live "$child" && [ "$i" -lt 50 ]; do i=$((i+1)); sleep .1; done
        if owned_live "$child"; then kill -KILL "$child" 2>/dev/null || true; fi
        wait "$child" 2>/dev/null || true
    done
    # Some guest socket backends leave the path behind when the owned bus
    # exits. Remove only the exact socket created by this supervisor, and
    # only if no bus has subsequently appeared there.
    if [ -n "$bus_pid" ] && [ -n "$bus_inode" ] &&
       [ "$(stat -Lc '%d:%i' /run/dbus/system_bus_socket 2>/dev/null || true)" = "$bus_inode" ] && ! bus_responds; then
        rm -f /run/dbus/system_bus_socket
    fi
}
trap cleanup EXIT
trap 'exit 129' HUP
trap 'exit 130' INT
trap 'exit 143' TERM
bus_responds() {
    dbus-send --system --reply-timeout=3000 --print-reply --dest=org.freedesktop.DBus \
        /org/freedesktop/DBus org.freedesktop.DBus.ListNames >/dev/null 2>&1
}
logind_responds() {
    dbus-send --system --reply-timeout=3000 --print-reply --dest=org.freedesktop.DBus \
        /org/freedesktop/DBus org.freedesktop.DBus.NameHasOwner string:org.freedesktop.login1 2>/dev/null | grep -q 'boolean true'
}
[ "$(id -u)" = 0 ] || { echo 'GNOME_SERVICES: root supervisor required'; exit 1; }
command -v dbus-daemon >/dev/null && command -v dbus-send >/dev/null || { echo 'GNOME_SERVICES: system D-Bus tools missing'; exit 1; }
system_bus_running() {
    for cmdline in /proc/[0-9]*/cmdline; do
        tr '\0' ' ' < "$cmdline" 2>/dev/null | grep -q '^[^ ]*dbus-daemon .*--system' && return 0
    done
    return 1
}
# /run lives in the persistent root filesystem. When iOS ends the app (or the
# runtime stops) while a desktop runs, the bus dies without unlinking its
# socket, and every later GNOME start found a dead socket. A socket with no
# live system dbus-daemon behind it is stale: remove it and start a bus. A
# live but slow daemon gets a short grace period and is never replaced.
if [ -S /run/dbus/system_bus_socket ] && ! bus_responds; then
    if system_bus_running; then
        i=0
        until bus_responds; do
            i=$((i+1)); [ "$i" -lt 25 ] || { echo 'GNOME_SERVICES: existing system bus is not responding'; exit 1; }; sleep .2
        done
    else
        echo 'GNOME_SERVICES: removing stale system bus socket (no system dbus-daemon running)'
        rm -f /run/dbus/system_bus_socket /run/dbus/pid
    fi
fi
if [ -S /run/dbus/system_bus_socket ]; then
    echo 'GNOME_SERVICES: using existing system bus'
else
    mkdir -p /run/dbus
    dbus-daemon --system --nofork --nopidfile > "$session/gnome-system-bus.log" 2>&1 & bus_pid=$!
    i=0
    until [ -S /run/dbus/system_bus_socket ] && bus_responds; do
        if [ -z "$bus_inode" ] && [ -S /run/dbus/system_bus_socket ]; then
            bus_inode=$(stat -Lc '%d:%i' /run/dbus/system_bus_socket 2>/dev/null || true)
        fi
        kill -0 "$bus_pid" 2>/dev/null || { echo 'GNOME_SERVICES: system bus exited'; exit 1; }
        i=$((i+1)); [ "$i" -lt 150 ] || { echo 'GNOME_SERVICES: system bus timeout'; exit 1; }; sleep .2
    done
    bus_inode=$(stat -Lc '%d:%i' /run/dbus/system_bus_socket 2>/dev/null || true)
    echo 'GNOME_SERVICES: started owned system bus'
fi
if logind_responds; then
    echo 'GNOME_SERVICES: using existing logind'
else
    [ -x /usr/lib/systemd/systemd-logind ] || { echo 'GNOME_SERVICES: logind missing'; exit 1; }
    /usr/lib/systemd/systemd-logind > "$session/gnome-logind.log" 2>&1 & logind_pid=$!
    i=0
    until logind_responds; do
        kill -0 "$logind_pid" 2>/dev/null || { echo 'GNOME_SERVICES: logind exited'; exit 1; }
        i=$((i+1)); [ "$i" -lt 150 ] || { echo 'GNOME_SERVICES: logind timeout'; exit 1; }; sleep .2
    done
    echo 'GNOME_SERVICES: started owned logind'
fi
su -l -s /bin/sh -c "exec /bin/sh '$support/session.sh' '$session' '$mode'" "$user" & user_pid=$!
status=0
wait "$user_pid" || status=$?
user_pid=
exit "$status"
