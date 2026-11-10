#!/bin/sh
# Emit only the app-managed effects block. Other user Lua is never replaced.
# A real GPU device is selected by the session, not inferred from the preset.
set -eu
profile=${1:-visual}
case "$profile" in performance|balanced|visual) ;; *) echo 'Invalid Hyprland effects profile' >&2; exit 2;; esac
if [ "${2:-}" != /dev/dri/card1 ] || [ "$profile" = performance ]; then
 printf '%s\n' 'hl.config({ decoration = { shadow = { enabled = false }, blur = { enabled = false } }, animations = { enabled = false } })'
elif [ "$profile" = balanced ]; then
 printf '%s\n' 'hl.config({ decoration = { shadow = { enabled = true, range = 8, render_power = 2, color = 0x66000000 }, blur = { enabled = false } }, animations = { enabled = true } })'
else
 printf '%s\n' 'hl.config({ decoration = { shadow = { enabled = true, range = 14, render_power = 3, color = 0x66000000 }, blur = { enabled = true, size = 6, passes = 2, vibrancy = 0.17 } }, animations = { enabled = true } })'
fi
