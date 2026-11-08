#!/usr/bin/env python3
"""Build and run the capture lifetime contract in a disposable minimal root.

Requires an already built ARM64 host interpreter and fakefsify tool, plus clang
and an ELF-capable ld.lld. No downloaded rootfs, guest libc or guest compiler.
"""
import argparse
import os
from pathlib import Path
import shutil
import subprocess
import tarfile
import tempfile

REPO = Path(__file__).resolve().parents[2]
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--runtime", type=Path, default=Path("build/host/ish"))
parser.add_argument("--fakefsify", type=Path, default=Path("build/host/tools/fakefsify"))
parser.add_argument("--output", type=Path, default=Path("build/capture-shm-test"))
args = parser.parse_args()
os.chdir(REPO)
runtime, fakefsify, output = (p.resolve() for p in (args.runtime, args.fakefsify, args.output))
output.mkdir(parents=True, exist_ok=True)
compiler = os.environ.get("CAPTURE_CC", "clang")
linker = os.environ.get("CAPTURE_LD") or shutil.which("ld.lld")
if not linker:
    parser.error("set CAPTURE_LD to an ELF-capable ld.lld")
flags = [compiler, "--target=aarch64-linux-gnu", f"--ld-path={linker}", "-O2",
         "-ffreestanding", "-fno-builtin", "-fno-stack-protector", "-nostdinc",
         "-nostdlib", "-static", "-Wall", "-Wextra", "-Werror", "-I.",
         "-Wl,-e,_start", "-Wl,--build-id=none", "-Wl,-z,noexecstack"]
guard_source = "app/LinuxRunner/GUISupport.bundle/capture-shm-guard.c"
for name, source, extra in [
    ("capture-contract", "tests/linuxrunner/capture_shm_contract.c", []),
    ("guard-unsupported", guard_source, ["-DCAPTURE_SHM_REQUEST_VERSION=0"]),
]:
    subprocess.run([*flags, *extra, source, "-o", str(output / name)], check=True)
subprocess.run(["bash", "scripts/build-capture-shm-guard.sh", str(output / "capture-shm-guard")],
               env={**os.environ, "CAPTURE_CC": compiler, "CAPTURE_LD": linker}, check=True)
bundled = REPO / "app/LinuxRunner/GUISupport.bundle/capture-shm-guard.arm64"
if bundled.read_bytes() != (output / "capture-shm-guard").read_bytes():
    raise SystemExit("Bundled capture guard differs from reproducible build")
with tarfile.open(output / "minimal.tar", "w") as archive:
    for directory in ("bin", "dev", "proc", "tmp", "etc"):
        info = tarfile.TarInfo(directory)
        info.type, info.mode = tarfile.DIRTYPE, 0o755
        archive.addfile(info)
    for name in ("capture-contract", "capture-shm-guard", "guard-unsupported"):
        archive.add(output / name, arcname=f"bin/{name}")
with tempfile.TemporaryDirectory(prefix="root-", dir=output) as temporary:
    root = Path(temporary) / "fs"
    subprocess.run([str(fakefsify), str(output / "minimal.tar"), str(root)], check=True)
    with (output / "contract.log").open("w") as log:
        subprocess.run([str(runtime), "-f", str(root), "/bin/capture-contract"],
                       stdout=log, stderr=subprocess.STDOUT, timeout=120, check=True)
lines = (output / "contract.log").read_text().splitlines()
passed = [line for line in lines if line.startswith("CAPTURE_SHM_CONTRACT_PASS ")]
if len(passed) != 1 or any("CAPTURE_SHM_FAIL" in line for line in lines):
    raise SystemExit(f"Contract did not pass; inspect {output / 'contract.log'}")
print(passed[0])
