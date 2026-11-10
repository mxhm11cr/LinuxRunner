#!/bin/sh
# Managed desktop first start, under the configured regular Linux user.
# Use the distribution's XDG initializer; never force/reset existing choices.
set -eu
[ "$(id -u)" != 0 ] || { echo 'DESKTOP_USER_DIRS: regular Linux user required'; exit 3; }
[ -n "${HOME:-}" ] && [ -d "$HOME" ] || { echo 'DESKTOP_USER_DIRS: configured home directory is missing'; exit 3; }
config=${XDG_CONFIG_HOME:-$HOME/.config}/user-dirs.dirs
if [ -e "$config" ] || [ -L "$config" ]; then
 echo 'DESKTOP_USER_DIRS: existing user configuration preserved'
 exit 0
fi
command -v xdg-user-dirs-update >/dev/null || { echo 'DESKTOP_USER_DIRS: install xdg-user-dirs through Desktop Support'; exit 3; }
xdg-user-dirs-update
echo 'DESKTOP_USER_DIRS: distribution first-start initializer completed'
