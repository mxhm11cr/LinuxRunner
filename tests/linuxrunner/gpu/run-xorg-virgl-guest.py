#!/usr/bin/env python3
"""Probe Ubuntu Xorg/glamor on virgl in a disposable macOS guest fixture.

Requires xserver-xorg-core, xauth, xsetroot, xwd and the guest's Mesa virgl driver. This
creates guest device nodes and a short-lived X server. It does not test a
desktop, visible frame, input, iOS hardware or application-level GL.
"""
import argparse
import fcntl
import json
import os
from pathlib import Path
import secrets
import shutil
import struct
import subprocess

parser = argparse.ArgumentParser(description=__doc__)
for name in ("runtime", "rootfs", "output", "lease", "angle-lib", "virgl-lib"):
    parser.add_argument("--" + name, type=Path, required=True)
parser.add_argument("--glxgears", action="store_true", help="also run and capture the guest's real glxgears")
args = parser.parse_args()
runtime, root = args.runtime.resolve(strict=True), args.rootfs.resolve(strict=True)
angle, virgl = args.angle_lib.resolve(strict=True), args.virgl_lib.resolve(strict=True)
output = args.output.absolute()
if output.exists() or any(c in str(output) for c in (",", "\n")):
    parser.error("output must be a new path without commas or newlines")
output.mkdir(parents=True)
repo = Path(__file__).resolve().parents[3]
shutil.copy2(repo / "app/LinuxRunner/GUISupport.bundle/steamos-xorg.conf", output / "xorg.conf")
env = dict(os.environ, ISH_VIRGL="1", ISH_SOFTWARE_DRM="1",
           DYLD_LIBRARY_PATH=f"{angle}:{virgl}",
           ISH_BIND_MOUNTS=f"/home/mobile/Downloads={output}")
script = r'''set -eu
mkdir -p /dev/dri
test -c /dev/dri/card1 || mknod -m 660 /dev/dri/card1 c 226 1
test -c /dev/dri/renderD129 || mknod -m 660 /dev/dri/renderD129 c 226 129
display=19093
test ! -e "/tmp/.X11-unix/X$display" || { echo XORG_DISPLAY_IN_USE; exit 1; }
export XAUTHORITY=/home/mobile/Downloads/authority
umask 077
xauth -f "$XAUTHORITY" add ":$display" . "$1"
Xorg ":$display" -config /home/mobile/Downloads/xorg.conf -logfile /home/mobile/Downloads/xorg-server.log -noreset -nolisten tcp -novtswitch -sharevts -auth "$XAUTHORITY" > /home/mobile/Downloads/xorg-output.log 2>&1 &
pid=$! gears_pid=
trap '[ -z "$gears_pid" ] || kill "$gears_pid" 2>/dev/null || true; kill "$pid" 2>/dev/null || true; wait "$pid" 2>/dev/null || true' EXIT
i=0
while [ "$i" -lt 100 ]; do
  if [ -S "/tmp/.X11-unix/X$display" ]; then
    echo XORG_SOCKET_READY
    DISPLAY=":$display" xsetroot -solid red
    DISPLAY=":$display" xwd -root -silent -out /home/mobile/Downloads/xorg-root.xwd
    echo XORG_ROOT_CAPTURED
    if [ "$2" = gears ]; then
      DISPLAY=":$display" glxgears -info > /home/mobile/Downloads/glxgears.log 2>&1 & gears_pid=$!
      sleep 8
      kill -0 "$gears_pid" 2>/dev/null || { echo GLXGEARS_EXITED; exit 1; }
      DISPLAY=":$display" xwininfo -root -tree > /home/mobile/Downloads/glxgears-windows.log
      DISPLAY=":$display" xwd -root -silent -out /home/mobile/Downloads/glxgears.xwd
      DISPLAY=":$display" xwd -name glxgears -silent -out /home/mobile/Downloads/glxgears-window.xwd || true
      if command -v lr-scanout-probe >/dev/null; then
        lr-scanout-probe /dev/dri/card1 /home/mobile/Downloads/scanout.raw from-host > /home/mobile/Downloads/scanout.log 2>&1 || true
        DISPLAY=":$display" xwd -root -silent -out /home/mobile/Downloads/glxgears-after-sync.xwd || true
      fi
      echo GLXGEARS_CAPTURED
    fi
    exit 0
  fi
  kill -0 "$pid" 2>/dev/null || { echo XORG_EXITED; exit 1; }
  sleep .1; i=$((i+1))
done
echo XORG_TIMEOUT; exit 1
'''
with args.lease.open("a") as lease, (output / "run.log").open("w") as log:
    fcntl.flock(lease, fcntl.LOCK_EX)
    result = subprocess.run([str(runtime), "-f", str(root), "/bin/sh", "-ec",
                             script, "xorg-virgl-probe", secrets.token_hex(16), "gears" if args.glxgears else "none"],
                            env=env, stdout=log, stderr=subprocess.STDOUT, timeout=60)
server_log = (output / "xorg-server.log")
lines = server_log.read_text(errors="replace").splitlines() if server_log.exists() else []
evidence = [line for line in lines if "glamor X acceleration enabled on" in line]
from xwd_capture import red_root_capture, different_window
red_capture = red_root_capture(output / "xorg-root.xwd")
gears_visible = different_window(output / "xorg-root.xwd", output / "glxgears.xwd") if args.glxgears else None
gears_after_sync = different_window(output / "xorg-root.xwd", output / "glxgears-after-sync.xwd") if args.glxgears and (output / "glxgears-after-sync.xwd").exists() else None
base_passed = (result.returncode == 0 and red_capture and any("virgl" in line.lower() and "angle" in line.lower()
                                                              and "metal" in line.lower() for line in evidence))
gears_log = output / "glxgears.log"
gears_renderer = [line for line in gears_log.read_text(errors="replace").splitlines()
                  if line.startswith("GL_RENDERER") ] if gears_log.exists() else []
gears_metal = any(all(word in line.lower() for word in ("virgl", "angle", "metal")) for line in gears_renderer)
passed = base_passed and (not args.glxgears or (gears_visible and gears_metal))
report = {"scope": "macOS Xorg/GLX pixel fixture; not desktop/input or iOS verification",
          "glxgears_metal_renderer": gears_metal if args.glxgears else None,
          "process_exit": result.returncode, "virgl_glamor_and_red_capture": base_passed,
          "red_root_capture": red_capture, "glxgears_visible": gears_visible,
          "glxgears_visible_after_test_sync": gears_after_sync, "evidence": evidence + gears_renderer}
(output / "result.json").write_text(json.dumps(report, indent=2) + "\n")
print(json.dumps(report, indent=2))
raise SystemExit(0 if passed else 1)
