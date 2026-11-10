#!/bin/sh
# Exercise the production package adapter in isolation: no real package writes.
# Each manager must receive only its own names, retain verification defaults,
# propagate failure, and reject a missing Fedora adapter before invoking DNF.
set -u
profile=${1:-$(dirname "$0")/../../../app/LinuxRunner/GUISupport.bundle/desktop-profile.sh}
profile=$(cd "$(dirname "$profile")" && pwd)/$(basename "$profile")
eval "$(sed -n '/^classify_failure() {/,/^}/p' "$profile")"
eval "$(sed -n '/^install_packages() {/,/^}/p' "$profile")"
work=$(mktemp -d) || exit 2
trap 'rm -rf "$work"' EXIT
cd "$work" || exit 2
apt_log=$work/package.log
stage() { printf 'STAGE %s\n' "$1"; }
have() { [ "$1" = "$manager" ]; }
record() { printf '%s\n' "$*" >> "$work/invocations"; }
mkdir "$work/bin"
cat > "$work/bin/apt-get" <<'MOCK'
#!/bin/sh
printf '%s\n' "$*" >> "$LR_ADAPTER_TEST_WORK/invocations"
MOCK
chmod +x "$work/bin/apt-get"
export LR_ADAPTER_TEST_WORK=$work
PATH=$work/bin:$PATH
export PATH
dpkg() { return 0; }
apk() { record "$@"; return 0; }
pacman() { record "$@"; return 0; }
dnf() { record "$@"; if [ "$1" = check ] && [ "${health_fail:-0}" = 1 ]; then echo "missing dependency: gstreamer1"; return 1; fi; if [ "${dnf_fail:-0}" != 0 ]; then echo 'GPG check FAILED'; return 1; fi; }
fails=0
for manager in apt-get apk pacman dnf; do
 rm -f invocations
 if install_packages test deb-only -- alpine-only -- arch-only -- fedora-only > result 2>&1; then
  case "$manager" in apt-get) expected=deb-only;; apk) expected=alpine-only;; pacman) expected=arch-only;; dnf) expected=fedora-only;; esac
  names=$(tr ' ' '\n' < invocations | grep -- '-only$')
  if [ "$names" = "$expected" ] && ! grep -E -- '--nogpgcheck|--allowerasing|--nodeps|--overwrite|--allow-unauthenticated' invocations; then
   echo "PASS isolated package names: $manager"
  else echo "FAIL cross-distribution names: $manager"; fails=$((fails+1)); fi
 else cat result; echo "FAIL adapter: $manager"; fails=$((fails+1)); fi
done
manager=dnf; rm -f invocations
if install_packages unsupported deb-only -- alpine-only -- arch-only > result 2>&1 || [ -e invocations ]; then
 echo 'FAIL missing adapter invoked DNF'; fails=$((fails+1))
else echo 'PASS absent Fedora adapter fails before package writes'; fi
dnf_fail=1
if install_packages failure deb-only -- alpine-only -- arch-only -- fedora-only > result 2>&1 || ! grep -q 'category=REPOSITORY_METADATA_ERROR' result; then
 cat result; echo 'FAIL DNF signature failure lost'; fails=$((fails+1))
else echo 'PASS signature failure propagated'; fi
# An executable left by a failed transaction must not imply a ready desktop.
eval "$(sed -n '/^check() {/,/^}/p' "$profile")"
gui_missing() { return 1; }
gui_helpers_stale() { return 1; }
desktop_missing() { return 1; }
audio_missing() { return 1; }
accounts_missing() { return 1; }
distro=fedora; dnf_fail=0; health_fail=1
check > result 2>&1
if grep -qx 'missing=package-database' profile-check.result; then
 echo 'PASS installed commands do not conceal failed RPM dependencies'
else cat result; echo 'FAIL unhealthy RPM database reported ready'; fails=$((fails+1)); fi
health_fail=0
check > result 2>&1
if grep -qx ready profile-check.result; then
 echo 'PASS current healthy database clears obsolete blocked state'
else cat result; echo 'FAIL healthy database stays blocked'; fails=$((fails+1)); fi
# The Fedora fallback uses GTK components; Arch keeps its existing Qt stack.
eval "$(sed -n '/^desktop_commands_present() {/,/^}/p' "$profile")"
have() { case " $available " in *" $1 "*) return 0;; *) return 1;; esac; }
available='openbox xterm pcmanfm lxpanel'; distro=fedora
if desktop_commands_present; then echo 'PASS Fedora GTK desktop adapter'; else fails=$((fails+1)); fi
distro=arch
if desktop_commands_present; then echo 'FAIL Arch incorrectly accepts GTK-only stack'; fails=$((fails+1)); else echo 'PASS Arch Qt requirement preserved'; fi
available='openbox xterm pcmanfm-qt lxqt-panel'
if desktop_commands_present; then echo 'PASS Arch Qt stack accepted'; else fails=$((fails+1)); fi
eval "$(sed -n '/^gui_helpers_stale() {/,/^}/p' "$profile")"
if gui_helpers_stale; then echo 'PASS missing fingerprint tools cannot mark helpers current'; else fails=$((fails+1)); fi
[ "$fails" = 0 ]
