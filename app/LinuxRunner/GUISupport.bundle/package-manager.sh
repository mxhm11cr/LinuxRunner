#!/bin/sh
# Literal argv from native UI. No eval, shell expressions or arbitrary options.
set -eu
umask 077
cd "$(dirname "$0")"
. ./support-common.sh
LR_PACMAN_RECOVERY_SCRIPT="$PWD/pacman-recover.sh"
. ./package-lock.sh
verify_resources
operation=${1:-}; value=${2:-}
case "$operation" in installed|search|details|updates|update|install|upgrade|remove|diagnose|configure|dependencies|repair) ;; *) echo 'PACKAGE_OPERATION: invalid operation'; exit 2;; esac
case "$operation" in
 search|details|install|upgrade|remove)
  [ -n "$value" ] && [ "${#value}" -le 128 ] || { echo 'PACKAGE_OPERATION: empty or oversized name'; exit 2; }
  case "$value" in -*|*[!a-zA-Z0-9+._:-]*) echo 'PACKAGE_OPERATION: invalid name'; exit 2;; esac;;
esac
case "$operation" in installed|search|details|updates|diagnose|dependencies|configure|repair) LR_PACKAGE_DIAGNOSIS=1;; esac
package_lock_acquire || exit $?
for file in package-operation.log package-result package-state.tsv package-details.txt package-interrupted package-operation.previous.log package-output.tmp package-search.tmp package-installed.tmp package-state.tsv.new package-details.txt.new package-result.new; do
 [ ! -L "$file" ] || { echo 'PACKAGE_OPERATION: unsafe managed file'; exit 2; }
done
if command -v apt-get >/dev/null; then distro=debian
elif command -v pacman >/dev/null; then distro=archarm
elif command -v apk >/dev/null; then distro=alpine
else echo 'PACKAGE_OPERATION: unsupported distribution'; exit 2; fi
if [ "$(sed -n 's/^ID=//p' /etc/os-release | tr -d '\"')" = steamos ]; then
 case "$operation" in installed|details|diagnose|dependencies) ;;
  *) echo 'PACKAGE_OPERATION: The SteamOS image experiment supports installed-package inspection. Package changes and repository updates are not enabled for this prepared image.'; exit 2;;
 esac
fi
printf 'operation=%s\ndistribution=%s\nstarted=%s\n' "$operation" "$distro" "$(date +%s)" > package-interrupted
printf 'running\n' > package-result
rm -f package-state.tsv.new package-details.txt.new
mv -f package-operation.log package-operation.previous.log 2>/dev/null || true
: > package-operation.log
perform_arch() {
 case "$operation" in
 installed)
  pacman -Q > package-installed.tmp
  awk 'BEGIN{OFS="\t"}{print $1,$2,"aarch64","installed"}' package-installed.tmp > package-state.tsv.new
  head -n 2000 package-state.tsv.new; mv package-state.tsv.new package-state.tsv; rm -f package-installed.tmp;;
 search)
  code=0; pacman -Ss --color never -- "$value" > package-search.tmp || code=$?
  # pacman uses exit 1 for no matches, and for errors: only empty output is a
  # successful empty result. stderr remains in the operation log.
  [ "$code" = 0 ] || { [ "$code" = 1 ] && [ ! -s package-search.tmp ]; } || return "$code"
  awk 'BEGIN{OFS="\t"} /^[^ \t]+\/[^ \t]+ / {split($1,a,"/");n=a[2];v=$2;s=($0~/\[installed/?"installed":"available");next} /^[ \t]/ && n!="" {sub(/^[ \t]+/,"");print n,v,"aarch64",s,$0;n=""}' package-search.tmp > package-state.tsv.new
  cat package-state.tsv.new; mv package-state.tsv.new package-state.tsv; rm -f package-search.tmp;;
 details)
  pacman -Si -- "$value" > package-details.txt.new || pacman -Qi -- "$value" > package-details.txt.new
  cat package-details.txt.new; mv package-details.txt.new package-details.txt;;
 updates)
  code=0; pacman -Qu > package-search.tmp || code=$?
  cat package-search.tmp
  [ "$code" = 0 ] || { [ "$code" = 1 ] && [ ! -s package-search.tmp ]; };;
 update) pacman -Sy;;
 install|upgrade)
  [ "$operation" != upgrade ] || pacman -Q -- "$value"
  # Rolling Arch does not support partial upgrades. The real full transaction
  # and any replacements are reviewed at pacman's interactive prompt.
  echo 'PACKAGE_STAGE: Arch synchronizes and upgrades the system to keep dependencies consistent; review the pacman transaction below'
  pacman -Syu --needed -- "$value";;
 remove) pacman -R -- "$value";;
 diagnose|dependencies)
  pacman -Dk || return $?
  if [ -e /usr/bin/gcc ]; then
   pacman -Q gcc && pacman -Qo /usr/bin/gcc || return $?
  fi;;
 configure|repair)
  /bin/bash "$LR_PACMAN_RECOVERY_SCRIPT";;
 esac
}
# tee streams the retained stdout/stderr into the terminal; the status is saved
# separately because POSIX pipelines do not propagate the first command's exit.
perform() {
 echo "PACKAGE_OPERATION: distribution=$distro operation=$operation started=$(date +%s)"
 if [ "$distro" = archarm ]; then perform_arch; return $?; fi
 case "$operation" in
 installed)
  if [ "$distro" = debian ]; then
   dpkg-query -W -f='${binary:Package}\t${Version}\t${Architecture}\t${db:Status-Abbrev}\n' > package-state.tsv.new
  else
   awk 'BEGIN {RS="";FS="\n";OFS="\t"} {n="";v="";a="";for(i=1;i<=NF;i++){k=substr($i,1,2);x=substr($i,3);if(k=="P:")n=x;if(k=="V:")v=x;if(k=="A:")a=x}if(n!="")print n,v,a,"installed"}' /lib/apk/db/installed > package-state.tsv.new
  fi
  head -n 2000 package-state.tsv.new; mv package-state.tsv.new package-state.tsv;;
 search)
  if [ "$distro" = debian ]; then
   apt-cache search --names-only -- "$value" > package-search.tmp
   set --
   while read -r name rest; do
    [ "${#name}" -le 128 ] || continue
    case "$name" in ''|-*|*[!a-zA-Z0-9+._:-]*) continue;; esac
    set -- "$@" "$name"; [ "$#" -lt 200 ] || break
   done < package-search.tmp
   dpkg-query -W -f='${Package}\t${Version}\t${Architecture}\t${db:Status-Abbrev}\n' > package-installed.tmp
   if [ "$#" -gt 0 ]; then
    apt-cache --no-all-versions show -- "$@" > package-search.tmp
    awk 'BEGIN{FS="\t";OFS="\t"} NR==FNR {installed[$1]=$4;next} /^Package: / {p=substr($0,10);v="";a="";d=""} /^Version: / {v=substr($0,10)} /^Architecture: / {a=substr($0,15)} /^Description(-[a-zA-Z_]+)?: / {d=$0;sub(/^[^:]*: /,"",d)} /^$/ {if(p!=""){print p,v,a,(p in installed?installed[p]:"not installed"),d;p=""}} END {if(p!="")print p,v,a,(p in installed?installed[p]:"not installed"),d}' package-installed.tmp package-search.tmp > package-state.tsv.new
   else : > package-state.tsv.new; fi
   rm -f package-installed.tmp
  else
   apk search -q -- "$value*" > package-search.tmp
   awk 'BEGIN{OFS="\t"} NR<=200 {print $0,"","see details","available"}' package-search.tmp > package-state.tsv.new
  fi
  cat package-state.tsv.new; mv package-state.tsv.new package-state.tsv; rm -f package-search.tmp;;
 details)
  if [ "$distro" = debian ]; then apt-cache show -- "$value" > package-details.txt.new; dpkg-query -W -f='Installed state: ${Status}\n' "$value" >> package-details.txt.new 2>/dev/null || true
  else apk info -a -- "$value" > package-details.txt.new; fi
  cat package-details.txt.new; mv package-details.txt.new package-details.txt;;
 updates)
  if [ "$distro" = debian ]; then apt list --upgradable; else apk version -l '<'; fi;;
 update)
  if [ "$distro" = debian ]; then apt-get update; else apk update; fi;;
 install)
  if [ "$distro" = debian ]; then DEBIAN_FRONTEND=noninteractive apt-get install --no-install-recommends --no-remove -y -- "$value"; else apk add -- "$value"; fi;;
 upgrade)
  # Upgrade only the named package. Never perform an automatic system upgrade
  # or accept removal of conflicting packages as an installation side effect.
  if [ "$distro" = debian ]; then
   # APT skips its prompt when only explicitly named packages change. Always
   # review its real plan before permitting even that trivial upgrade.
   apt-get --simulate install --only-upgrade --no-install-recommends --no-remove -- "$value"
   echo 'Confirm named-package upgrade by typing yes:'
   read -r answer; [ "$answer" = yes ] || return 1
   apt-get install --only-upgrade --no-install-recommends --no-remove -- "$value"
  else apk upgrade --simulate -- "$value"; echo 'Confirm APK upgrade by typing yes:'; read -r answer; [ "$answer" = yes ] && apk upgrade -- "$value"; fi;;
 remove)
  # APT displays the actual affected packages and asks for confirmation.
  if [ "$distro" = debian ]; then apt-get remove -- "$value"; else apk del --simulate -- "$value"; echo 'Confirm APK removal by typing yes:'; read -r answer; [ "$answer" = yes ] && apk del -- "$value"; fi;;
 diagnose)
  cat /proc/ish/tasks
  if [ "$distro" = debian ]; then dpkg --audit; apt-get check; else apk audit --system; fi
  for file in /etc/passwd /etc/group /etc/shadow /etc/gshadow; do [ ! -e "$file" ] || stat -c 'ACCOUNT_METADATA: path=%n mode=%a uid=%u gid=%g' "$file"; done;;
 configure|repair)
  # Persistent metadata checkpoint only. Never claim a full rootfs snapshot.
  checkpoint=$(mktemp -d /var/lib/linuxrunner/repair-checkpoints/repair.XXXXXX)
  if [ "$distro" = debian ]; then cp -p /var/lib/dpkg/status "$checkpoint/dpkg-status"; dpkg --audit; dpkg --configure -a
   if [ "$operation" = repair ]; then apt-get -f install; fi
  else cp -p /lib/apk/db/installed "$checkpoint/apk-installed"; apk fix; fi
  echo "PACKAGE_REPAIR: metadata_checkpoint=$checkpoint rollback=manual-not-automatic";;
 dependencies)
  if [ "$distro" = debian ]; then apt-get check; else apk fix --simulate; fi;;
 esac
}
mkdir -p /var/lib/linuxrunner/repair-checkpoints
# Do not use set -e inside a pipeline/function as an error-status substitute.
# The subshell exits immediately on a real package-manager failure.
# POSIX asynchronous lists replace fd 0 with /dev/null before redirections.
# Preserve the terminal/pipe explicitly, so APT/APK confirmation remains real.
operation_signal=0
# A terminal interrupt must not release the managed lock while its worker
# still owns APT/APK state. The package manager also receives the terminal's
# process-group SIGINT and decides which step can safely be interrupted.
trap 'operation_signal=130' INT
exec 3<&0
(
 set -e
 perform
) <&3 3<&- > package-output.tmp 2>&1 & worker=$!
exec 3<&-
tail -n +1 -f package-output.tmp & reader=$!
code=0; wait "$worker" || code=$?
if [ "$operation_signal" -ne 0 ]; then
 # wait may return for a trapped signal before the worker has actually exited.
 while kill -0 "$worker" 2>/dev/null; do wait "$worker" || code=$?; done
 code=$operation_signal
 echo 'PACKAGE_OPERATION: interrupted; retained state requires diagnosis' >&2
fi
kill "$reader" 2>/dev/null || true; wait "$reader" 2>/dev/null || true
# Keep one previous tail and one current tail. No unbounded historical logs.
tail -c 1048576 package-output.tmp > package-operation.log
rm -f package-output.tmp
printf '\nPACKAGE_OPERATION: distribution=%s operation=%s exit=%s ended=%s\n' "$distro" "$operation" "$code" "$(date +%s)" >> package-operation.log
tail -n 20 package-operation.log
printf '%s\n' "$code" > package-result.new; mv package-result.new package-result
[ "$code" -ne 0 ] || rm -f package-interrupted
exit "$code"
