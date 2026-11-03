# Verified archive import

LinuxRunner snapshots an authenticated archive into a private, exclusive file,
checks its SHA-256 while copying, and extracts those exact bytes. Copying is
bounded, does not follow a final symlink, and supports cancellation. The generic
512 MiB limit remains; a prepared SteamOS archive additionally requires the
bundled manifest's architecture, source-image hash, archive hash and exact size
(up to 6 GiB). No SteamOS image or installer is introduced by this change.

PAX UTF-8 names may be unavailable in the host's C locale. The importer accepts
only libarchive's four exact PAX locale-conversion warnings, after validating the
original pathname, link and account-name bytes as UTF-8. Other warnings and codec
failures remain errors. The app does not change global or per-thread locale.
Every import exit closes descriptors, SQLite and archive state; failed staging
and private copies are removed. Cancellation returns NSUserCancelledError.

Run `tests/linuxrunner/steamos/run-root-import.sh` on macOS after initializing
the pinned libarchive submodule. It compiles the current importer and a static
libarchive with gzip support, using disposable directories and a test-only app
bundle. The fixture verifies Unicode file/symlink bytes in the C locale, locale
preservation, invalid UTF-8 and truncated gzip rejection, wrong SHA/source pin,
symlink rejection, cancellation during copy/extraction, successful publication,
and preservation of the source archive. It does not access the iOS app container.

The extracted PR passed this host fixture on Apple Silicon macOS with Xcode
clang. It has not been independently installed or tested on a physical iPad.
