#!/bin/sh
# Reversible SteamOS-only workaround for KIO 6.14 enumerating tags even when
# Baloo is disabled. Retain the original plugin outside Qt's plugin search tree.
set -eu
[ "$(id -u)" = 0 ] || { echo 'STEAMOS_TAGS: root-required'; exit 1; }
[ "$(sed -n 's/^ID=//p' /etc/os-release | tr -d '\"')" = steamos ] || exit 0
plugin=/usr/lib/qt6/plugins/kf6/kio/tags.so
backup=/opt/linuxrunner/steamos-disabled-plugins
action=${1:-}
case "$action" in
 tags-policy)
  user=${2:-}
  case "$user" in ''|*[!a-z0-9_-]*|[0-9-]*) exit 2;; esac
  home=$(awk -F: -v name="$user" '$1==name {print $6; count++} END {if(count!=1)exit 1}' /etc/passwd)
  case "$home" in /*) ;; *) exit 2;; esac
  # No override means the managed desktop's default (Baloo off). Preserve an
  # explicit account autostart override and put back our saved worker for it.
  override="$home/.config/autostart/baloo_file.desktop"
  action=disable-tags
  if [ -e "$override" ] || [ -L "$override" ]; then
   grep -qx 'Hidden=true' "$override" || action=restore-tags
  fi
  ;;
 disable-tags|restore-tags) ;;
 *) echo 'Usage: steamos-desktop.sh tags-policy USER | disable-tags | restore-tags'; exit 2;;
esac
for path in /opt /opt/linuxrunner "$backup" "$plugin" "$backup/tags.so"; do
 [ ! -L "$path" ] || { echo 'STEAMOS_TAGS: symlink refused'; exit 1; }
done
if [ "$action" = disable-tags ]; then
 if [ -f "$plugin" ]; then
  mkdir -p "$backup"
  # An unexpected second copy can be a package update; keep both untouched.
  [ ! -e "$backup/tags.so" ] || { echo 'STEAMOS_TAGS: plugin changed; retained copies, no override'; exit 0; }
  mv "$plugin" "$backup/tags.so"
 fi
 [ ! -f "$backup/tags.so" ] || echo 'STEAMOS_TAGS: unavailable worker disabled; original retained (restore-tags to undo)'
elif [ -f "$backup/tags.so" ]; then
 [ ! -e "$plugin" ] || { echo 'STEAMOS_TAGS: package worker already present; backup retained'; exit 0; }
 mv "$backup/tags.so" "$plugin"
 echo 'STEAMOS_TAGS: original worker restored'
fi
