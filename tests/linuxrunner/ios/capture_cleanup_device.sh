#!/bin/sh
# Explicit physical-device test companion. Run from the native terminal, never
# the desktop application group: it must survive the capture/session teardown.
# Operates only on a NEW LinuxRunner session created after this monitor starts.
set -eu
if [ "${1:-}" = all ]; then
    init_start=$(sed 's/.*) //' /proc/1/stat | cut -d ' ' -f 20)
    history=/home/mobile/Downloads/capture-cleanup-history
    fixed_baseline=/home/mobile/Downloads/capture-cleanup-baseline
    find /home/mobile/Downloads/.linuxrunner/gui -maxdepth 1 -type d -name 'session-*' > "$history"
    cat /proc/ish/shm > "$fixed_baseline"
    trap 'rm -f "$history" "$fixed_baseline"' EXIT
    cycle=0
    for phase in ready-kill ready-kill ready-kill ready-kill normal-stop; do
        cycle=$((cycle+1))
        /bin/sh "$0" "$cycle" "$phase" "$history" "$fixed_baseline" || exit 1
    done
    [ "$(sed 's/.*) //' /proc/1/stat | cut -d ' ' -f 20)" = "$init_start" ] || exit 1
    echo 'CAPTURE_CLEANUP_ALL_PASS same_runtime=yes cycles=5'
    exit 0
fi
cycle=${1:?cycle required}
phase=${2:?ready-kill, startup-kill or normal-stop required}
case "$cycle" in ''|*[!0-9]*) exit 2;; esac
case "$phase" in ready-kill|startup-kill|normal-stop) ;; *) exit 2;; esac
base=/home/mobile/Downloads/.linuxrunner/gui
out=/home/mobile/Downloads/capture-cleanup-$cycle
snapshot() { cat /proc/ish/shm; }
field() { printf '%s\n' "$1" | tr ' ' '\n' | sed -n "s/^$2=//p"; }
start_time() { sed 's/.*) //' "/proc/$1/stat" 2>/dev/null | cut -d ' ' -f 20; }
init_start=$(start_time 1)
baseline=$(snapshot)
[ -z "${4:-}" ] || baseline=$(cat "$4")
fail() { echo "cycle=$cycle result=FAIL reason=$1"; exit 1; }
if [ -n "${3:-}" ]; then
    history=$3
else
    history=$out.existing
    trap 'rm -f "$history"' EXIT
    find "$base" -maxdepth 1 -type d -name 'session-*' > "$history"
fi
printf 'baseline %s\n' "$baseline"
printf 'CAPTURE_MONITOR_ARMED_%s\n' "$cycle"
deadline=$(( $(date +%s) + 300 ))
session=
while [ "$(date +%s)" -lt "$deadline" ]; do
    for candidate in "$base"/session-*; do
        [ -f "$candidate/capture-process" ] || continue
        grep -Fxq "$candidate" "$history" && continue
        session=$candidate; break
    done
    [ -z "$session" ] || break
    sleep .2
done
[ -n "$session" ] || fail no-new-capture
printf '%s\n' "$session" >> "$history"
if [ "$phase" = startup-kill ]; then
    # Catch a real allocated buffer, not just a launcher PID before allocation.
    while [ "$(date +%s)" -lt "$deadline" ]; do
        [ "$(field "$(snapshot)" capture_segments)" -gt "$(field "$baseline" capture_segments)" ] && break
        sleep .05
    done
else
    while ! grep -qx 'ready=1' "$session/desktop-state" 2>/dev/null; do
        [ "$(date +%s)" -lt "$deadline" ] || fail desktop-not-ready
        sleep .2
    done
fi
# Leave the ready desktop visible long enough for the semantic UI readiness
# assertion before terminating it. No delay is used for the early-kill mode.
[ "$phase" = startup-kill ] || sleep 10
live=$(snapshot)
[ "$(field "$live" capture_segments)" -gt "$(field "$baseline" capture_segments)" ] || fail no-scoped-buffers
grep -qx 'GUI_CAPTURE: cleanup=process-scope' "$session/display.log" || fail no-capture-lease
printf 'live %s\n' "$live"
if [ "$phase" = normal-stop ]; then
    touch "$session/stop"
else
    read -r pid parent started < "$session/capture-process"
    case "$pid:$parent:$started" in *[!0-9:]*|'') fail invalid-identity;; esac
    [ "$pid" -gt 1 ] && [ "$parent" -gt 1 ] || fail invalid-pid
    [ "$(start_time "$pid")" = "$started" ] || fail reused-pid
    actual_parent=$(sed 's/.*) //' "/proc/$pid/stat" | cut -d ' ' -f 2)
    [ "$actual_parent" = "$parent" ] || fail wrong-parent
    kill -KILL "$pid"
fi
# All totals, including descriptors created and successfully closed, must return
# to the same baseline. Require a stable window, not one lucky sample.
stable=0
deadline=$(( $(date +%s) + 90 ))
while [ "$(date +%s)" -lt "$deadline" ]; do
    after=$(snapshot)
    if [ "$after" = "$baseline" ]; then stable=$((stable+1)); else stable=0; fi
    [ "$stable" -lt 5 ] || break
    sleep .2
done
[ "$stable" -ge 5 ] || fail resources-not-baseline
[ "$(start_time 1)" = "$init_start" ] || fail runtime-restarted
printf 'after %s\n' "$after"
echo "cycle=$cycle result=PASS phase=$phase same_runtime=yes"
echo "CAPTURE_CLEANUP_CYCLE_${cycle}_PASS"
