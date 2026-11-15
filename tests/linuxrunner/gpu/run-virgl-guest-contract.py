#!/usr/bin/env python3
"""Run the real virgl pixel and desktop-GL probes in a disposable guest on macOS.

The fixture must already have gcc and EGL, GLES, GL and GBM development
packages. This modifies its /dev/dri and /tmp; never point it at user data.
This is an off-screen test, not a desktop or iOS hardware qualification.
"""
import argparse
import fcntl
import json
import os
from pathlib import Path
import shutil
import subprocess

parser = argparse.ArgumentParser(description=__doc__)
for name in ("runtime", "rootfs", "output", "lease", "angle-lib", "virgl-lib"):
    parser.add_argument("--" + name, type=Path, required=True)
parser.add_argument("--guest-user", required=True)
args = parser.parse_args()
if not args.guest_user or not all(c.isascii() and (c.isalnum() or c in "_-") for c in args.guest_user):
    parser.error("guest user must be a simple Linux account name")
source = Path(__file__).resolve().parent
runtime, root = args.runtime.resolve(strict=True), args.rootfs.resolve(strict=True)
angle, virgl = args.angle_lib.resolve(strict=True), args.virgl_lib.resolve(strict=True)
output = args.output.absolute()
if output.exists() or any(c in str(output) for c in (",", "\n")):
    parser.error("output must be a new path without commas or newlines")
output.mkdir(parents=True, mode=0o755)
for file in ("virgl_render_contract.c", "virgl_gl_profile.c"):
    shutil.copy2(source / file, output / file)
env = dict(os.environ, ISH_VIRGL="1", ISH_SOFTWARE_DRM="1",
           DYLD_LIBRARY_PATH=f"{angle}:{virgl}",
           ISH_BIND_MOUNTS=f"/home/mobile/Downloads={output}")
script = """set -eu
cd /home/mobile/Downloads
mkdir -p /dev/dri
test -c /dev/dri/renderD129 || mknod -m 666 /dev/dri/renderD129 c 226 129
gcc -O2 virgl_render_contract.c -lEGL -lGLESv2 -lgbm -o /tmp/lr-virgl-pixel-test
gcc -O2 virgl_gl_profile.c -lEGL -lGL -lgbm -o /tmp/lr-virgl-gl-test
su -l -s /bin/sh -c /tmp/lr-virgl-pixel-test "$1"
code=0
su -l -s /bin/sh -c /tmp/lr-virgl-gl-test "$1" || code=$?
echo DESKTOP_GL_PROFILE_EXIT:$code
test "$code" = 0 || test "$code" = 2
"""
with args.lease.open("a") as lease, (output / "run.log").open("w") as log:
    fcntl.flock(lease, fcntl.LOCK_EX)
    result = subprocess.run([str(runtime), "-f", str(root), "/bin/sh", "-ec",
                             script, "virgl-contract", args.guest_user],
                            env=env, stdout=log, stderr=subprocess.STDOUT, timeout=180)
text = (output / "run.log").read_text(errors="replace")
pixel_pass = result.returncode == 0 and "VIRGL_RENDER_CONTRACT PASS" in text
renderer_lines = [line for line in text.splitlines() if line.startswith("VIRGL_RENDER_CONTRACT renderer=")]
metal_path = any("virgl" in line.lower() and "angle" in line.lower() and "metal" in line.lower()
                 for line in renderer_lines)
report = {"scope": "off-screen macOS guest test; not iOS or desktop verification",
          "process_exit": result.returncode, "pixel_pass": pixel_pass, "metal_path": metal_path,
          "evidence": [line for line in text.splitlines() if line.startswith(
              ("VIRGL_RENDER", "VIRGL_GL_PROFILE", "DESKTOP_GL_PROFILE_EXIT"))]}
(output / "result.json").write_text(json.dumps(report, indent=2) + "\n")
print(json.dumps(report, indent=2))
raise SystemExit(0 if pixel_pass and metal_path else 1)
