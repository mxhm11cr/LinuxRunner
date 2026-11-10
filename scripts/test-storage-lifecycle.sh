#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
output=$(mktemp -d "${TMPDIR:-/tmp}/linuxrunner-storage-tests.XXXXXX")
trap 'rm -rf "$output"' EXIT
# UIKit cannot be linked into a macOS executable. Extract the complete production
# manager and scanner, with stable boundaries that fail if the file is reorganized.
python3 - "$output/ManagerTests.swift" <<'PYTHON'
from pathlib import Path
import sys
source = Path('app/LinuxRunner/ManagerView.swift').read_text()
gate_start = source.index('@MainActor @objc(LRGraphicalActivity)')
gate_end = source.index('@MainActor final class EnvironmentManager:', gate_start)
Path(sys.argv[1]).with_name('GraphicalActivity.swift').write_text('import Foundation\nimport Combine\n' + source[gate_start:gate_end])
start = source.index('@MainActor final class EnvironmentManager:')
end = source.index('@MainActor @objc(LinuxRunnerShell)', start)
tests = Path('tests/linuxrunner/diagnostics/StorageLifecycleTests.swift').read_text()
Path(sys.argv[1]).write_text(tests + '\n' + source[start:end])
PYTHON
xcrun swiftc -swift-version 5 app/LinuxRunner/Environment.swift app/LinuxRunner/Diagnostics.swift \
    "$output/GraphicalActivity.swift" "$output/ManagerTests.swift" -o "$output/storage-lifecycle-tests"
"$output/storage-lifecycle-tests"
# Exercise the actual native pause predicate, pre-presentation hook and owner
# cleanup against the real Swift gate. The host has no UIKit, so supply a small
# superclass/client fixture; the explicit Swift bridge exposes the internal gate
# without changing app visibility or adding production test APIs.
python3 - "$output" <<'PYTHON'
from pathlib import Path
import sys
output = Path(sys.argv[1])
source = Path('app/LinuxRunner/GUIViewController.m').read_text()
methods = []
for start, end in [
    ('- (void)viewWillAppear:', '- (void)viewDidAppear:'),
    ('- (void)viewDidDisappear:', '- (void)updateStorageActivity {'),
    ('- (void)updateStorageActivity {', '- (BOOL)externalOutputActive'),
    ('- (void)dealloc{NSUUID *source=', '- (void)message:'),
]:
    begin = source.index(start)
    methods.append(source[begin:source.index(end, begin)])
native = Path('tests/linuxrunner/diagnostics/NativeStorageActivityTests.m').read_text()
native = native.replace('// PRODUCTION_STORAGE_ACTIVITY_METHODS', ''.join(methods))
native = native.replace('LRGraphicalActivity.shared', '((LRGraphicalActivity *)LRStorageTestState.activity)')
(output / 'Native.m').write_text(native)
(output / 'Bridge.swift').write_text('''import Foundation
@MainActor @objc(LRStorageTestState) public final class StorageTestState: NSObject {
    @objc public static var activity: NSObject { GraphicalActivity.shared }
    @objc public static var active: Bool { GraphicalActivity.shared.active }
}
''')
PYTHON
xcrun swiftc -swift-version 5 -emit-library -emit-objc-header \
    -emit-objc-header-path "$output/StorageFixture-Swift.h" -module-name StorageFixture \
    app/LinuxRunner/Environment.swift app/LinuxRunner/Diagnostics.swift "$output/GraphicalActivity.swift" "$output/Bridge.swift" \
    -o "$output/libStorageFixture.dylib"
test_target=$(xcrun swiftc -print-target-info | python3 -c 'import json, sys; print(json.load(sys.stdin)["target"]["triple"])')
xcrun clang -target "$test_target" -fobjc-arc -framework Foundation -I"$output" "$output/Native.m" \
    -L"$output" -lStorageFixture -Wl,-rpath,"$output" -o "$output/native-tests"
python3 - "$output/native-tests" <<'PYTHON'
import subprocess, sys
subprocess.run([sys.argv[1]], check=True, timeout=10)
PYTHON
xcrun swiftc -swift-version 5 app/LinuxRunner/Environment.swift app/LinuxRunner/Diagnostics.swift \
    tests/linuxrunner/diagnostics/DiagnosticsTests.swift -o "$output/diagnostics-tests"
"$output/diagnostics-tests"
