#!/bin/sh
# Actual distribution XDG initializer, regular Linux user, disposable HOME only.
set -eu
helper=$1
[ "$(id -u)" != 0 ] || exit 2
work=$(mktemp -d /tmp/lr-user-dirs-contract.XXXXXXXX)
trap 'rm -rf "$work"' EXIT HUP INT TERM
export HOME="$work/home" XDG_CONFIG_HOME="$work/config" LC_ALL=C.UTF-8
mkdir -p "$HOME" "$XDG_CONFIG_HOME"
/bin/sh "$helper"
test -d "$HOME/Documents" && test -d "$HOME/Downloads"
test -s "$XDG_CONFIG_HOME/user-dirs.dirs"
mkdir "$HOME/Custom"
printf 'XDG_DOCUMENTS_DIR="$HOME/Custom"\n# user customization\n' > "$XDG_CONFIG_HOME/user-dirs.dirs"
old=$(sha256sum "$XDG_CONFIG_HOME/user-dirs.dirs")
/bin/sh "$helper"
[ "$old" = "$(sha256sum "$XDG_CONFIG_HOME/user-dirs.dirs")" ]
printf 'XDG_DOCUMENTS_DIR="$HOME/NotPresent"\n' > "$XDG_CONFIG_HOME/user-dirs.dirs"
old=$(sha256sum "$XDG_CONFIG_HOME/user-dirs.dirs")
/bin/sh "$helper"
[ "$old" = "$(sha256sum "$XDG_CONFIG_HOME/user-dirs.dirs")" ]
[ ! -e "$HOME/NotPresent" ]
mv "$XDG_CONFIG_HOME/user-dirs.dirs" "$work/original"
ln -s "$work/original" "$XDG_CONFIG_HOME/user-dirs.dirs"
/bin/sh "$helper"
test -L "$XDG_CONFIG_HOME/user-dirs.dirs"
[ "$old" = "$(sha256sum "$XDG_CONFIG_HOME/user-dirs.dirs")" ]
echo DESKTOP_USER_DIRS_PASS first-start folders and exact custom/missing/symlink configuration preservation
