#!/bin/sh
# Read-only KWin integration-test observer for the existing managed session.
set -eu
set -a
. "$HOME/.cache/linuxrunner/desktop.env"
set +a
case "$1" in *[!a-z0-9-]*|'') exit 2;; esac
logs=/home/mobile/Downloads/logs
plugin=lr-window-test-$1
/home/mobile/Downloads/kwin-window-snapshot "$logs/kwin-windows-$1.json" > "$logs/kwin-oracle-$1.log" 2>&1 & monitor=$!
i=0; until gdbus call --session --dest org.freedesktop.DBus --object-path /org/freedesktop/DBus --method org.freedesktop.DBus.NameHasOwner org.linuxrunner.TestWindowWatch | grep -q true; do i=$((i+1)); [ "$i" -lt 30 ] || exit 3; sleep .1; done
trap 'kill "$monitor" 2>/dev/null || true; gdbus call --session --dest org.kde.KWin --object-path /Scripting --method org.kde.kwin.Scripting.unloadScript "$plugin" >/dev/null 2>&1 || true' EXIT HUP INT TERM
reply=$(gdbus call --session --dest org.kde.KWin --object-path /Scripting --method org.kde.kwin.Scripting.loadScript /home/mobile/Downloads/kwin-window-watch.js "$plugin")
id=$(printf '%s' "$reply" | sed 's/[^0-9]/ /g' | awk '{for(i=NF;i>0;i--)if($i~/^[0-9]+$/){print $i;exit}}')
[ -n "$id" ]
gdbus call --session --dest org.kde.KWin --object-path "/Scripting/Script$id" --method org.kde.kwin.Script.run
wait "$monitor"
