#!/usr/bin/env python3
"""Run the DRM contract with a fresh host interpreter and disposable guest root.
Build the interpreter with -Dvdso_required=true for guest signal return.

Requires Zig for a static ARM64 Linux test executable and a fakefsify tool.
"""
import argparse
import os
from pathlib import Path
import subprocess
import tarfile
import tempfile

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--runtime', type=Path, required=True)
parser.add_argument('--fakefsify', type=Path, required=True)
parser.add_argument('--zig', default='zig')
parser.add_argument('--output', type=Path, default=Path('build/drm-contract'))
args = parser.parse_args()
os.chdir(Path(__file__).resolve().parents[2])
output = args.output.resolve()
output.mkdir(parents=True, exist_ok=True)
# Create nodes through Linux syscalls: archive dev_t encoding is host-specific.
init = output / 'fixture-init.c'
init.write_text('''#include <sys/stat.h>
#include <unistd.h>
int main(void) {
    if (mknod("/dev/dri/card0", S_IFCHR | 0666, 226 << 8) ||
        mknod("/dev/dri/renderD128", S_IFCHR | 0666, (226 << 8) | 128)) return 2;
    execl("/bin/drm-contract", "drm-contract", (char *)0);
    return 3;
}
''')
for source, name in [(Path('tests/linuxrunner/software_drm_contract.c'), 'drm-contract'),
                     (init, 'fixture-init')]:
    subprocess.run([args.zig, 'cc', '-target', 'aarch64-linux-musl', '-static', '-O2',
                    '-Wall', '-Wextra', str(source), '-o', str(output / name)], check=True)
archive = output / 'minimal.tar'
with tarfile.open(archive, 'w') as tar:
    for name in ('bin', 'dev', 'dev/dri', 'proc', 'sys', 'tmp', 'etc'):
        info = tarfile.TarInfo(name)
        info.type, info.mode = tarfile.DIRTYPE, 0o755
        tar.addfile(info)
    for name in ('drm-contract', 'fixture-init'):
        tar.add(output / name, arcname='bin/' + name)
with tempfile.TemporaryDirectory(prefix='root-', dir=output) as temporary:
    root = Path(temporary) / 'fs'
    subprocess.run([str(args.fakefsify.resolve()), str(archive), str(root)], check=True)
    with (output / 'contract.log').open('w') as log:
        subprocess.run([str(args.runtime.resolve()), '-f', str(root), '/bin/fixture-init'],
                       env={**os.environ, 'ISH_SOFTWARE_DRM': '1'}, stdout=log,
                       stderr=subprocess.STDOUT, timeout=30, check=True)
lines = (output / 'contract.log').read_text().splitlines()
assert lines.count('DRM_CONTRACT PASS') == 1 and not any('DRM_CONTRACT FAIL' in line for line in lines)
print('DRM_CONTRACT PASS (idle clock, wait, async events, bounds and close-before-deadline)')
