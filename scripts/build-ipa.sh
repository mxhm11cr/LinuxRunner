#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=signed
diagnostic=NO
gui_test=NO
qt_test=NO
browser_test=NO
for option in "$@"; do
    case "$option" in
        --unsigned) mode=unsigned ;;
        --diagnostic) diagnostic=YES ;;
        --browser-test) browser_test=YES; qt_test=YES; gui_test=YES; diagnostic=YES ;;
        --qt-test) qt_test=YES; gui_test=YES; diagnostic=YES ;;
        --gui-test) gui_test=YES; diagnostic=YES ;;
        *) echo 'Usage: scripts/build-ipa.sh [--unsigned] [--diagnostic | --gui-test | --qt-test | --browser-test]' >&2; exit 2 ;;
    esac
done
artifact=LinuxRunner
if [[ $diagnostic == YES ]]; then artifact=LinuxRunner-Debug; fi
if [[ $gui_test == YES ]]; then artifact=LinuxRunner-GUI-Test; fi
if [[ $qt_test == YES ]]; then artifact=LinuxRunner-Qt-Test; fi
if [[ $browser_test == YES ]]; then artifact=LinuxRunner-Browser-Test; fi
artifact="${ARTIFACT_NAME:-$artifact}"
export DEVELOPER_DIR="${DEVELOPER_DIR:-/Applications/Xcode.app/Contents/Developer}"
python3 scripts/generate-gui-support-manifest.py --check
scripts/bootstrap-macos.sh
release="${RELEASE_DIR:-$PWD/build/releases}"
if [[ $diagnostic == YES && -z ${RELEASE_DIR:-} ]]; then release="$PWD/build/device-diagnostics/release"; fi
if [[ $gui_test == YES && -z ${RELEASE_DIR:-} ]]; then release="$PWD/build/gui/release"; fi
if [[ $qt_test == YES && -z ${RELEASE_DIR:-} ]]; then release="$PWD/build/qt/release"; fi
if [[ $browser_test == YES && -z ${RELEASE_DIR:-} ]]; then release="$PWD/build/browser/release"; fi
mkdir -p "$release"
release=$(cd "$release" && pwd)
archive="$release/$artifact.xcarchive"
cmake_path=${LINUXRUNNER_CMAKE:-$(command -v cmake || true)}
[[ -n $cmake_path ]] || { echo "CMake required for pinned shader compiler" >&2; exit 1; }
args=("LINUXRUNNER_CMAKE=$cmake_path" -project iSH.xcodeproj -scheme LinuxRunner -configuration Release
      -destination 'generic/platform=iOS' -archivePath "$archive"
      -derivedDataPath "$PWD/build/release-derived"
      "LINUXRUNNER_SOURCE_COMMIT=$(git rev-parse HEAD)"
      IPHONEOS_DEPLOYMENT_TARGET=15.0 "LINUXRUNNER_DIAGNOSTIC=$diagnostic"
      "ROOT_BUNDLE_IDENTIFIER=${BUNDLE_ID:-dev.linuxrunner.app}")
if [[ $diagnostic == YES ]]; then args+=(CODE_SIGN_ENTITLEMENTS=app/LinuxRunner/PrivateContainer.entitlements); fi
# Optional variants (app/AppARM64.xcconfig): JIT-capable runtime, GPU device
# (needs scripts/build-gpu-deps.sh iphoneos first). Both stay off until enabled in Settings.
if [[ ${LINUXRUNNER_JIT:-NO} == YES ]]; then args+=(LINUXRUNNER_JIT=YES); fi
if [[ ${LINUXRUNNER_VIRGL:-NO} == YES ]]; then
    test -f build/gpu/iphoneos/lib/libangle.a || { echo "GPU dependencies missing: run scripts/build-gpu-deps.sh iphoneos" >&2; exit 1; }
    args+=(LINUXRUNNER_VIRGL=YES)
fi
if [[ $mode == unsigned ]]; then
    args+=(CODE_SIGNING_ALLOWED=NO CODE_SIGNING_REQUIRED=NO)
else
    : "${TEAM_ID:?Set TEAM_ID to your own Apple team identifier, or use --unsigned for an IPA to sign yourself.}"
    if ! security find-identity -v -p codesigning | python3 -c 'import re,sys; sys.exit(not re.search(r"\b[1-9][0-9]* valid identities found",sys.stdin.read()))'; then
        echo 'No valid Apple signing identity in the keychain. Configure signing in Xcode or use --unsigned.' >&2; exit 1
    fi
    args+=("DEVELOPMENT_TEAM=$TEAM_ID" CODE_SIGN_STYLE=Automatic)
    if [[ ${ALLOW_PROVISIONING_UPDATES:-0} == 1 ]]; then args+=(-allowProvisioningUpdates); fi
fi
if ! xcodebuild "${args[@]}" archive 2>&1 | tee "$release/archive.log"; then
    echo "Archive failed. Inspect $release/archive.log. Signing requires your own team, app identifier and a matching provisioning profile (app group optional with private-container fallback)." >&2
    exit 1
fi
ipa="$release/$artifact.ipa"
if [[ $mode == unsigned ]]; then
    staging=$(mktemp -d "$release/.unsigned-XXXXXX")
    trap 'rm -rf "$staging"' EXIT
    mkdir "$staging/Payload"
    app="$archive/Products/Applications/LinuxRunner.app"
    test -d "$app" || { echo "Missing archived app: $app" >&2; exit 1; }
    # Ad-hoc signature with the resolved entitlements. Not an Apple signature: it
    # gives installers that keep or extend an existing signature (TrollStore,
    # LiveContainer, AltSign/zsign-based tools) a well-formed code signature and
    # entitlements to work from; re-signing tools replace it. 1.0.0 shipped with
    # no signature at all.
    python3 - "$staging/entitlements.plist" "${BUNDLE_ID:-dev.linuxrunner.app}" "$diagnostic" <<'PYADHOC'
import plistlib, sys
with open(sys.argv[1], 'wb') as out:
    plistlib.dump({} if sys.argv[3] == 'YES' else {'com.apple.security.application-groups': ['group.' + sys.argv[2]]}, out)
PYADHOC
    # Retain exactly the signed executable shipped in the IPA, so the archive
    # provenance check remains byte-for-byte strict after ad-hoc signing.
    codesign --force --sign - --timestamp=none --generate-entitlement-der --entitlements "$staging/entitlements.plist" "$app"
    codesign --verify --strict "$app"
    ditto --norsrc --noextattr --noacl --noqtn "$app" "$staging/Payload/LinuxRunner.app"
    codesign --verify --strict "$staging/Payload/LinuxRunner.app"
    # Info-ZIP: sizes in local headers (no data descriptors) and no platform extra
    # fields, which streaming ZIP readers in some installers require.
    (cd "$staging" && zip -q -r -X -y LinuxRunner.ipa Payload)
    mv "$staging/LinuxRunner.ipa" "$ipa"
    python3 scripts/validate-ipa.py "$ipa" --report "$release/$artifact.validation.json" ||
        { echo 'IPA validation failed; do not publish this artifact.' >&2; exit 1; }
    printf 'UNSIGNED IPA (ad-hoc signed, no Apple signature). Requires signing and provisioning before physical-device installation.\nBundle ID: %s\nApp group: group.%s\nEmbedded extensions: none in this ARM64 scheme\nPrivate Application Support fallback: supported\n' \
        "${BUNDLE_ID:-dev.linuxrunner.app}" "${BUNDLE_ID:-dev.linuxrunner.app}" > "$release/SIGNING-STATUS.txt"
else
    options="${EXPORT_OPTIONS_PLIST:-$release/ExportOptions.plist}"
    if [[ -z ${EXPORT_OPTIONS_PLIST:-} ]]; then
        python3 - "$options" "$TEAM_ID" "${EXPORT_METHOD:-debugging}" <<'PY'
import plistlib,sys
with open(sys.argv[1], 'wb') as f:
    plistlib.dump({'method':sys.argv[3], 'teamID':sys.argv[2], 'signingStyle':'automatic', 'stripSwiftSymbols':True},f)
PY
    fi
    export_args=(-exportArchive -archivePath "$archive" -exportPath "$release/export" -exportOptionsPlist "$options")
    if [[ ${ALLOW_PROVISIONING_UPDATES:-0} == 1 ]]; then export_args+=(-allowProvisioningUpdates); fi
    if ! xcodebuild "${export_args[@]}" 2>&1 | tee "$release/export.log"; then
        echo "IPA export failed. Check $release/export.log and your ExportOptions.plist signing method/profiles." >&2; exit 1
    fi
    exported="$release/export/LinuxRunner.ipa"
    test -f "$exported" || { echo 'Xcode did not produce LinuxRunner.ipa.' >&2; exit 1; }
    cp "$exported" "$ipa"
    printf 'Signed by Xcode export method %s. Verify on the intended physical device.\n' "${EXPORT_METHOD:-custom ExportOptions.plist}" > "$release/SIGNING-STATUS.txt"
fi
python3 - "$release/$artifact.entitlements.plist" "${BUNDLE_ID:-dev.linuxrunner.app}" "$diagnostic" <<'PYENT'
import plistlib, sys
with open(sys.argv[1], 'wb') as out:
    plistlib.dump({} if sys.argv[3] == 'YES' else {'com.apple.security.application-groups': ['group.' + sys.argv[2]]}, out)
PYENT
(cd "$release" && shasum -a 256 "$artifact.ipa" > "$artifact.ipa.sha256")
git rev-parse HEAD > "$release/source-revision.txt"
git status --short >> "$release/source-revision.txt"
git submodule status --recursive >> "$release/source-revision.txt"
python3 - "$release/source-files.sha256" <<'PYHASH'
import hashlib, pathlib, subprocess, sys
names = subprocess.check_output(['git', 'ls-files', '-z', '--cached', '--others', '--exclude-standard']).split(b'\0')
with open(sys.argv[1], 'w') as out:
    for name in sorted(set(names)):
        if not name: continue
        path = pathlib.Path(name.decode())
        if path.is_file():
            out.write(f'{hashlib.sha256(path.read_bytes()).hexdigest()}  {path}\n')
PYHASH
printf 'IPA: %s\nSigning: %s\nArchive and symbols: %s\n' "$ipa" "$mode" "$archive"
