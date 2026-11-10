#!/bin/sh
# desktop-profile.sh classify_failure: apt/dpkg output → the category Open
# Desktop shows. Includes the exact messages of the IPA 890 device log.
# Runs on any POSIX shell (host or guest): sh classify_failure_test.sh [desktop-profile.sh]
profile=${1:-$(dirname "$0")/../../../app/LinuxRunner/GUISupport.bundle/desktop-profile.sh}
eval "$(sed -n '/^classify_failure() {/,/^}/p' "$profile")"
work=$(mktemp -d) || exit 2; apt_log=$work/apt; fails=0
# expect CATEGORY "install log" ["update log"]
expect() {
    printf '%s\n' "$2" > "$apt_log"; rm -f "$apt_log.update"; [ -z "${3:-}" ] || printf '%s\n' "$3" > "$apt_log.update"
    got=$(classify_failure test | sed -n 's/^PROFILE_FAILURE: category=\([A-Z_]*\).*/\1/p')
    if [ "$got" = "$1" ]; then echo "PASS $1"; else echo "FAIL expected $1 got ${got:-none}: $2"; fails=$((fails+1)); fi
}
expect PACKAGE_MANAGER_BUSY 'dpkg: error: dpkg frontend lock was locked by /usr/bin/dpkg process with pid 6
E: Sub-process /usr/bin/dpkg returned an error code (2)'
expect PACKAGE_MANAGER_BUSY 'E: Could not get lock /var/lib/dpkg/lock-frontend. It is held by process 6 (dpkg)'
expect TLS_CERTIFICATE_ERROR 'Err:1 https://deb.debian.org/debian trixie InRelease
  SSL connection failed: certificate verify failed
E: The repository is not signed.'
expect TLS_CERTIFICATE_ERROR 'Err:2 https://security.debian.org/debian-security trixie-security InRelease
  Certificate verification failed: The certificate is NOT trusted. The certificate issuer is unknown.'
# Offline: update "succeeds" with warnings, the install cannot find packages.
expect NETWORK_UNAVAILABLE 'E: Unable to locate package xvfb' "Err:1 https://deb.debian.org/debian trixie InRelease
  Temporary failure resolving 'deb.debian.org'
W: Failed to fetch https://deb.debian.org/debian/dists/trixie/InRelease  Temporary failure resolving 'deb.debian.org'
W: Some index files failed to download. They have been ignored, or old ones used instead."
# Same, but the download failed on certificates.
expect TLS_CERTIFICATE_ERROR 'E: Package openbox has no installation candidate' 'Err:1 https://deb.debian.org/debian trixie InRelease
  SSL connection failed: certificate verify failed
W: Failed to fetch https://deb.debian.org/debian/dists/trixie/InRelease  SSL connection failed: certificate verify failed'
# A package that really is missing (lists downloaded fine).
expect PACKAGE_NOT_FOUND 'E: Unable to locate package no-such-package' 'Get:1 https://deb.debian.org/debian trixie InRelease [140 kB]
Reading package lists...'
expect DEPENDENCY_CONFLICT 'The following packages have unmet dependencies:
 xserver-xorg-core : Depends: libgl1 but it is not going to be installed
E: Unable to correct problems, you have held broken packages.'
expect INSUFFICIENT_STORAGE "E: You don't have enough free space in /var/cache/apt/archives/."
expect PACKAGE_DATABASE_RECOVERY_REQUIRED "E: dpkg was interrupted, you must manually run 'dpkg --configure -a' to correct the problem."
expect REPOSITORY_METADATA_ERROR 'Err:3 https://deb.debian.org/debian trixie InRelease
  404  Not Found [IP: 151.101.2.132 443]
E: The repository does not have a Release file.'
expect FILESYSTEM_ERROR 'dpkg: error: unable to create new file /var/lib/dpkg/status-new: Read-only file system'
expect PACKAGE_NOT_FOUND 'No match for argument: missing-fedora-package'
expect DEPENDENCY_CONFLICT 'Problem: conflicting requests
 - nothing provides missing-lib needed by requested-package'
expect REPOSITORY_METADATA_ERROR 'GPG check FAILED'
expect FILESYSTEM_FEATURE_UNSUPPORTED 'unpacking of archive failed on file /usr/libexec/gstreamer-1.0/gst-ptp-helper: cpio: cap_set_file failed - Operation not supported'
rm -rf "$work"
echo "CLASSIFY_FAILURE_TEST $([ $fails = 0 ] && echo PASS || echo FAIL) failures=$fails"
[ $fails = 0 ]
