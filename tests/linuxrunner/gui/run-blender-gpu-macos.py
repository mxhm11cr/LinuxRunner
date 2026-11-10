#!/usr/bin/env python3
"""Qualify an installed ARM64 Blender GUI in a disposable Mac rootfs.

Does not install packages, rebuild ANGLE, or claim device performance. Supply
DYLD_LIBRARY_PATH for the preserved host ANGLE/virgl libraries. The CLI runtime
must support virgl. Use the shared heavy-test lease for this workspace.
"""
import argparse
import fcntl
import json
import os
from pathlib import Path
import shutil
import subprocess

parser = argparse.ArgumentParser(description=__doc__)
for key in ("runtime", "rootfs", "output", "lease"):
    parser.add_argument("--" + key, type=Path, required=True)
parser.add_argument("--guest-user", required=True)
parser.add_argument("--seconds", type=int, default=240)
args = parser.parse_args()
if not 30 <= args.seconds <= 600:
    parser.error("seconds must be between 30 and 600")
source = Path(__file__).resolve().parents[3]
output = args.output.absolute()
if output.exists():
    parser.error("choose a new output directory")
if any(c in str(output) for c in (",", "\n")):
    parser.error("CLI bind path contains a delimiter")
runtime = args.runtime.resolve(strict=True)
rootfs = args.rootfs.resolve(strict=True)
output.mkdir(parents=True)
env = dict(os.environ, DEVELOPER_DIR="/Applications/Xcode.app/Contents/Developer",
           ISH_VIRGL="1", ISH_SOFTWARE_DRM="1", ISH_SOFTWARE_DMABUF="1")

with args.lease.open("a") as lease:
    fcntl.flock(lease, fcntl.LOCK_EX)
    probe = output / "probe"
    with (output / "probe-build.log").open("w") as log:
        subprocess.run(["xcrun", "clang", "-O2", "-fobjc-arc", "-I.",
                        "tests/linuxrunner/gui/blender_gpu_capture_probe.m",
                        "app/LinuxRunner/RFBClient.m", "app/LinuxRunner/RFBProtocol.c",
                        "-framework", "Foundation", "-framework", "CoreGraphics",
                        "-framework", "ImageIO", "-o", str(probe)], cwd=source,
                       env=env, stdout=log, stderr=subprocess.STDOUT, check=True)
    with (output / "prepare-render-node.log").open("w") as log:
        subprocess.run([str(runtime), "-f", str(rootfs), "/bin/sh", "-ec",
                        "mkdir -p /dev/dri; test -c /dev/dri/renderD129 || "
                        "mknod /dev/dri/renderD129 c 226 129; chmod 666 /dev/dri/renderD129; "
                        "test -x /usr/bin/blender"], env=env, stdout=log,
                       stderr=subprocess.STDOUT, timeout=30, check=True)
    rows = []
    for mode, software in (("hardware-request", "0"), ("software-baseline", "1")):
        exchange = output / ("exchange-" + mode)
        exchange.mkdir(mode=0o777)
        exchange.chmod(0o777)  # Disposable guest-user exchange, no private data.
        shutil.copy2(source / "tests/linuxrunner/gui/blender_gpu_benchmark.py", exchange / "benchmark.py")
        script = exchange / "launch-blender.sh"
        script.write_text("set -eu\n"
                          "export LR_BLENDER_RESULTS=/home/mobile/Downloads/benchmark.json\n"
                          "export LR_BLENDER_UI_READY=/home/mobile/Downloads/viewer.ready\n"
                          f"export LIBGL_ALWAYS_SOFTWARE={software}\n"
                          "unset QT_OPENGL GALLIUM_DRIVER MESA_LOADER_DRIVER_OVERRIDE\n"
                          "exec blender --factory-startup -noaudio --debug-gpu "
                          "--python-exit-code 45 --python /home/mobile/Downloads/benchmark.py\n")
        # Host chmod does not set fakefs's guest metadata. Set ownership using
        # the same bind path the session will use, before starting that session.
        with (output / (mode + "-ownership.log")).open("w") as log:
            subprocess.run([str(runtime), "-f", str(rootfs), "/bin/sh", "-ec",
                            'id "$1"; chown "$1" /home/mobile/Downloads; chmod 700 /home/mobile/Downloads',
                            "blender-test-owner", args.guest_user],
                           env=dict(env, ISH_BIND_MOUNTS="/home/mobile/Downloads=" + str(exchange)),
                           stdout=log, stderr=subprocess.STDOUT, timeout=30, check=True)
        with (output / (mode + "-driver.log")).open("w") as log:
            run = subprocess.run(["python3", "tests/linuxrunner/gui/run_guest_session.py",
                                  "--runtime", str(runtime), "--rootfs", str(rootfs),
                                  "--exchange", str(exchange), "--mode", "custom",
                                  "--script", str(script), "--guest-user", args.guest_user,
                                  "--prepare-support", "--glx", "distribution", "--geometry", "960x540",
                                  "--probe", str(probe), "--output", str(output / mode),
                                  "--timeout", str(args.seconds + 30)], cwd=source,
                                 env=dict(env, LR_BLENDER_HOST_RESULTS=str(exchange / "benchmark.json"),
                                          LR_BLENDER_HOST_UI_READY=str(exchange / "viewer.ready"),
                                          LR_BLENDER_SECONDS=str(args.seconds)),
                                 stdout=log, stderr=subprocess.STDOUT, timeout=args.seconds + 90)
        report_path = exchange / "benchmark.json"
        report = json.loads(report_path.read_text()) if report_path.exists() else None
        rows.append(dict(mode=mode, exit=run.returncode, benchmark=report,
                         hardware_verified=bool(report and report.get("status") == "PASS"
                                                and "virgl" in report.get("renderer", "").lower()
                                                and "angle" in report.get("renderer", "").lower()),
                         note="GPU request is not proof that the selected X11 path uses hardware"))
        (output / "results.json").write_text(json.dumps(rows, indent=2) + "\n")
        print(mode, run.returncode, flush=True)
