#!/usr/bin/env python3
"""Native Mac Metal regression. Requires the existing ANGLE/virgl libraries.

The caller holds the heavy-job lease. --expect-timeout is only for reproducing
the old failure from a saved host-source file, not a successful rendering test.
"""
import argparse
import os
from pathlib import Path
import subprocess

p = argparse.ArgumentParser(description=__doc__)
for name in ('angle-root', 'angle-lib', 'virgl-prefix', 'output'):
    p.add_argument('--' + name, type=Path, required=True)
p.add_argument('--host-source', type=Path)
p.add_argument('--expect-timeout', action='store_true')
a = p.parse_args()
root = Path(__file__).resolve().parents[3]
a.output.mkdir(parents=True, exist_ok=False)
program = a.output / 'fence-failure'
command = ['xcrun', 'clang', '-std=c11', '-Wall', '-Wextra', '-DEGL_EGLEXT_PROTOTYPES', '-I' + str(root),
           '-I' + str(a.angle_root / 'include'), '-I' + str(a.virgl_prefix / 'include'),
           str(a.host_source or root / 'graphics/virgl_host.c'),
           str(root / 'tests/linuxrunner/gpu/virgl_fence_failure.c'),
           '-L' + str(a.angle_lib), '-L' + str(a.virgl_prefix / 'lib'),
           '-lEGL', '-lGLESv2', '-lvirglrenderer', '-pthread', '-o', str(program)]
with (a.output / 'build.log').open('w') as log:
    subprocess.run(command, stdout=log, stderr=subprocess.STDOUT, check=True)
env = dict(os.environ, DYLD_LIBRARY_PATH=f'{a.angle_lib}:{a.virgl_prefix / "lib"}')
timed_out = False
with (a.output / 'run.log').open('w') as log:
    try:
        result = subprocess.run([str(program)], env=env, stdout=log,
                                stderr=subprocess.STDOUT, timeout=15)
    except subprocess.TimeoutExpired:
        timed_out = True
text = (a.output / 'run.log').read_text()
if a.expect_timeout:
    assert timed_out and 'injecting allocation error after real GPU completion' in text, text
    print('VIRGL_FENCE_FAILURE original worker hang reproduced after injected allocation failure')
else:
    assert not timed_out and result.returncode == 0 and 'VIRGL_FENCE_FAILURE PASS' in text, text
    print('VIRGL_FENCE_FAILURE PASS (native Mac Metal; not iOS hardware validation)')
