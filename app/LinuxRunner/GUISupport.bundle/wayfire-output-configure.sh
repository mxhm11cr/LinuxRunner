#!/bin/sh
# Update only the session-managed output mode; retain all user output settings.
set -eu
config=$1 output=$2 geometry=$3
case "$output" in X11-1|Virtual-1) ;; *) exit 2;; esac
case "$geometry" in *[!0-9x]*|x*|*x|*x*x*|'') exit 2;; esac
case "$geometry" in *x*) ;; *) exit 2;; esac
# Respect user-managed links instead of replacing them with a regular file.
if [ -L "$config" ]; then echo 'WAYFIRE_OUTPUT: symlink preserved'; exit 0; fi
[ -f "$config" ] || exit 1
mode=$geometry
[ "$output" != Virtual-1 ] || mode="$geometry@60000"
temp=$(mktemp "$(dirname "$config")/.linuxrunner-wayfire-output.XXXXXXXX")
trap 'rm -f "$temp"' EXIT HUP INT TERM
cp -p "$config" "$temp"
awk -v section="[output:$output]" -v mode="$mode" '
    /^\[/ {
        if (inside && !written) print "mode = " mode
        inside = ($0 == section)
        if (inside) found = 1
    }
    inside && /^[[:space:]]*mode[[:space:]]*=/ {
        if (!written) { print "mode = " mode; written = 1 }
        next
    }
    { print }
    END {
        if (inside && !written) print "mode = " mode
        if (!found) { print ""; print section; print "mode = " mode }
    }
' "$config" > "$temp"
mv "$temp" "$config"
echo "WAYFIRE_OUTPUT: output=$output mode=$mode"
