#!/usr/bin/env python3
"""Audit every Mach-O in a device IPA and match its retained archive/symbols."""
import hashlib, json, os, pathlib, plistlib, re, stat, subprocess, sys, tempfile, zipfile
os.environ.setdefault('DEVELOPER_DIR', '/Applications/Xcode.app/Contents/Developer')
release = pathlib.Path(sys.argv[1] if len(sys.argv) > 1 and not sys.argv[1].startswith('--') else 'build/browser/release' if '--browser-test' in sys.argv else 'build/qt/release' if '--qt-test' in sys.argv else 'build/gui/release' if '--gui-test' in sys.argv else 'build/device-diagnostics/release' if '--diagnostic' in sys.argv else 'build/releases')
name = 'LinuxRunner-Browser-Test' if '--browser-test' in sys.argv else 'LinuxRunner-Qt-Test' if '--qt-test' in sys.argv else 'LinuxRunner-GUI-Test' if '--gui-test' in sys.argv else 'LinuxRunner-Debug' if '--diagnostic' in sys.argv else 'LinuxRunner'
name = os.environ.get('ARTIFACT_NAME', name)
ipa = release / (name + '.ipa')
def xcrun(*args):
    return subprocess.check_output(['xcrun', *args], text=True).strip()
with zipfile.ZipFile(ipa) as archive:
    assert archive.testzip() is None, 'Corrupt IPA'
    metadata_entries = [n for n in archive.namelist() if any(part.startswith('._') or part == '__MACOSX' for part in pathlib.PurePosixPath(n).parts)]
    assert not metadata_entries, f'Mac metadata in IPA: {metadata_entries[:3]}'
    app = 'Payload/LinuxRunner.app/'
    info = plistlib.loads(archive.read(app + 'Info.plist'))
    executable_hash = hashlib.sha256(archive.read(app + info['CFBundleExecutable'])).hexdigest()
    root_hash = hashlib.sha256(archive.read(app + 'root.tar.gz')).hexdigest()
    mode = archive.getinfo(app + info['CFBundleExecutable']).external_attr >> 16
    assert mode & stat.S_IXUSR, 'Executable permission missing'
    assert root_hash == info['LinuxRunnerRootfsSHA256'], 'Rootfs pin mismatch'
    assert info['CFBundleSupportedPlatforms'] == ['iPhoneOS'], 'Simulator artifact'
    assert info['UIDeviceFamily'] == [1, 2], 'Expected iPhone and iPad'
    if '--gui-test' in sys.argv or '--qt-test' in sys.argv or '--browser-test' in sys.argv:
        resources = ('gui-test.c','install.sh','session.sh')
        if '--qt-test' in sys.argv or '--browser-test' in sys.argv: resources += ('qt-test.cpp','focus-manager.c','install-qt.sh','launch.sh','network-probe.c','regression.sh')
        if '--browser-test' in sys.argv: resources += ('browser.cpp','browser-test.html','firefox-test.html','install-browser.sh','browser-launch.sh','browser-preflight.sh','support-common.sh','installer-run.sh','support-manifest.json','support.sha256','support-version')
        assert all(app+'GUISupport.bundle/'+n in archive.namelist() for n in resources), 'GUI support resources missing'
    source_tree = os.environ.get('LINUXRUNNER_VERIFY_SOURCE_TREE')
    expected_commit = os.environ.get('LINUXRUNNER_EXPECTED_SOURCE_COMMIT')
    if expected_commit:
        embedded_commit = info.get('LinuxRunnerSourceCommit', '')
        assert len(embedded_commit) >= 8 and expected_commit.startswith(embedded_commit), 'Unexpected embedded source commit'
    manifest=json.loads(archive.read(app+'GUISupport.bundle/support-manifest.json'))
    for resource in manifest['resources']:
        contents=archive.read(app+'GUISupport.bundle/'+resource['name'])
        assert hashlib.sha256(contents).hexdigest()==resource['sha256'], 'Bundled support-resource checksum mismatch: '+resource['name']
        if source_tree:
            canonical = pathlib.Path(source_tree) / 'app/LinuxRunner/GUISupport.bundle' / resource['name']
            assert canonical.read_bytes() == contents, 'IPA/canonical support-resource mismatch: '+resource['name']
    # A normal release may have a descriptive ARTIFACT_NAME. Only diagnostic
    # schemes require diagnostic launch; the filename does not select a scheme.
    if any(flag in sys.argv for flag in ('--diagnostic', '--gui-test', '--qt-test', '--browser-test')):
        assert str(info.get('LinuxRunnerDiagnostic', '')).lower() in ('yes', 'true', '1'), 'Diagnostic launch disabled'
    result = dict(size_bytes=ipa.stat().st_size, ipa_sha256=hashlib.sha256(ipa.read_bytes()).hexdigest(),
        bundle_id=info['CFBundleIdentifier'], display_name=info['CFBundleDisplayName'], version=info['CFBundleShortVersionString'], build=info['CFBundleVersion'],
        source_commit=info.get('LinuxRunnerSourceCommit'), canonical_resource_count=len(manifest['resources']),
        canonical_resources_match=True if source_tree else None,
        minimum_ios=info['MinimumOSVersion'], supported_platforms=info['CFBundleSupportedPlatforms'],
        device_families=info['UIDeviceFamily'], rootfs_sha256=root_hash, rootfs_pin_matches=True,
        diagnostic_launch=info.get('LinuxRunnerDiagnostic'), executable_mode=oct(mode),
        embedded_extensions=[n for n in archive.namelist() if '.appex/Info.plist' in n],
        has_embedded_profile=any(n.endswith('embedded.mobileprovision') for n in archive.namelist()))
binary = release / (name + '.xcarchive/Products/Applications/LinuxRunner.app/LinuxRunner')
dsym = release / (name + '.xcarchive/dSYMs/LinuxRunner.app.dSYM')
assert xcrun('lipo', '-archs', str(binary)) == 'arm64', 'Expected ARM64 executable'
result['platform'] = xcrun('vtool', '-show-build', str(binary))
result['binary_uuid'] = xcrun('dwarfdump', '--uuid', str(binary))
result['dsym_uuid'] = xcrun('dwarfdump', '--uuid', str(dsym))
assert result['binary_uuid'].split()[1] == result['dsym_uuid'].split()[1], 'dSYM mismatch'
result['signature_check'] = subprocess.run(['codesign','-dv',str(binary.parent)], capture_output=True,text=True).stderr.strip()
result['ipa_executable_matches_archive'] = executable_hash == hashlib.sha256(binary.read_bytes()).hexdigest()
assert result['ipa_executable_matches_archive'], 'IPA/archive mismatch'
options = pathlib.Path('build/release-derived/Build/Intermediates.noindex/ArchiveIntermediates/LinuxRunner/BuildProductsPath/Release-iphoneos/meson/meson-info/intro-buildoptions.json')
if options.exists():
    values = {item['name']:item['value'] for item in json.loads(options.read_text())}
    result['execution_build_options'] = {k:values[k] for k in ('guest_arch','jit','jit_emit','cli_aot')}
    # Interpreter-only unless the release is declared as the JIT-capable variant
    # (LINUXRUNNER_JIT=YES, as scripts/build-ipa.sh builds it); never AOT.
    jit_variant = os.environ.get('LINUXRUNNER_JIT') == 'YES'
    result['jit_variant_declared'] = jit_variant
    assert values['guest_arch'] == 'arm64' and not values['cli_aot']
    assert values['jit'] == values['jit_emit'] == jit_variant, 'JIT build options do not match the declared variant'
# GPU variant: libepoxy looks EGL up with dlsym, so the entry points must be
# exported from the shipped (stripped) executable or enabling the GPU aborts.
result['gpu_variant'] = b'virtio-gpu device active' in binary.read_bytes()
if result['gpu_variant']:
    exports = xcrun('dyld_info', '-exports', str(binary))
    result['gpu_egl_exports'] = sum(1 for line in exports.splitlines() if re.search(r' _egl[A-Z]', line))
    assert ' _eglGetProcAddress' in exports and ' _eglGetCurrentDisplay' in exports, 'GPU variant without exported EGL entry points'
result['recursive_macho_audit'] = []
with tempfile.TemporaryDirectory(prefix='linuxrunner-ipa-audit-') as directory:
    with zipfile.ZipFile(ipa) as archive: archive.extractall(directory)
    root = pathlib.Path(directory) / 'Payload/LinuxRunner.app'
    for path in sorted(root.rglob('*')):
        if not path.is_file() or path.is_symlink(): continue
        with path.open('rb') as source: magic = source.read(4)
        if magic not in (b'\xcf\xfa\xed\xfe', b'\xfe\xed\xfa\xcf', b'\xca\xfe\xba\xbe', b'\xbe\xba\xfe\xca', b'\xca\xfe\xba\xbf'): continue
        arch = xcrun('lipo','-archs',str(path))
        platform = xcrun('vtool','-show-build',str(path))
        assert set(arch.split()) <= {'arm64','arm64e'}, f'Non-device architecture: {path}'
        assert 'platform IOS\n' in platform and 'SIMULATOR' not in platform, f'Wrong platform: {path}'
        linked = xcrun('otool','-L',str(path)).splitlines()[1:]
        assert not any('/Users/' in line or 'DerivedData' in line or 'Simulator' in line for line in linked), 'Host dependency leaked'
        commands = xcrun('otool','-l',str(path)).splitlines()
        rpaths = [commands[i+2].strip() for i,line in enumerate(commands) if 'cmd LC_RPATH' in line]
        result['recursive_macho_audit'].append(dict(path=str(path.relative_to(root)), architectures=arch,
            platform=platform.replace(str(path),str(path.relative_to(root))), linked_libraries=linked, rpaths=rpaths))
assert result['recursive_macho_audit'], 'No Mach-O found'
(release / 'artifact-verification.json').write_text(json.dumps(result,indent=2)+'\n')
print(json.dumps(result,indent=2))
