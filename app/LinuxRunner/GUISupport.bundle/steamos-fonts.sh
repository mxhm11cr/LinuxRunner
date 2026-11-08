#!/bin/sh
# Managed default for a scaled virtual display, run as the selected Linux user.
# RGB subpixel antialiasing assumes a fixed physical RGB stripe arrangement;
# it produces colored edges when the framebuffer is resized or rotated.
set -eu
config=${XDG_CONFIG_HOME:-$HOME/.config}
file="$config/kdeglobals"
if [ -L "$file" ] || { [ -e "$file" ] && [ ! -f "$file" ]; }; then
    echo 'STEAMOS_FONTS: custom configuration path preserved'
    exit 0
fi
# KConfig handles the write. Conservatively preserve any explicit account key,
# including immutable/expanded forms, even when it is in another group.
if [ -f "$file" ] && grep -Eq '^[[:space:]]*XftSubPixel(\[\$[ie]\])*[[:space:]]*=' "$file"; then
    echo 'STEAMOS_FONTS: explicit subpixel preference preserved'
    exit 0
fi
if ! command -v kwriteconfig6 >/dev/null; then
    echo 'STEAMOS_FONTS: configuration tool unavailable; defaults unchanged'
    exit 0
fi
mkdir -p "$config" || { echo 'STEAMOS_FONTS: configuration directory unavailable; defaults unchanged'; exit 0; }
if kwriteconfig6 --file "$file" --group General --key XftSubPixel none; then
    echo 'STEAMOS_FONTS: grayscale antialiasing default for scaled framebuffer'
else
    echo 'STEAMOS_FONTS: configuration write unavailable; defaults unchanged'
fi
