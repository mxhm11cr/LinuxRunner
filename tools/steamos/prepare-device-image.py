#!/usr/bin/env python3
"""Prepare a disposable SteamFrame fakefs import for LinuxRunner's device installer.

Requires a case-sensitive --root from export-steamframe.py, a host interpreter,
and the fakefs exporter. Adds pinned display packages, builds guest helpers,
and exports the prepared archive and its app-side manifest.
"""
import argparse
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import subprocess
import tarfile
import urllib.request

SOURCE_PIN = "3a4a077f1b1f40688ab3279affcb56776bd97c54db1573e7c65fc52a97106676"
# SHA-256 and detached signatures checked against the Arch Linux ARM Build
# System key 68B3537F39A313B3E574D06777193F152BDBE6A6.
PACKAGES = {
    "x11vnc-1:0.9.17-1-aarch64.pkg.tar.xz": "28f757a6b8f495eda9a0c4d6385ad68d72ab143e5f0e613885ff8ca95cc58549",
    "libvncserver-0.9.15-1-aarch64.pkg.tar.xz": "c941976dedf7898dd485a6880b83162a13b143e3c33f9c808ccd4b2896c0fe4d",
}
HELPERS = ("gui-test", "focus-manager", "desktop-probe", "window-observer")
REPO = Path(__file__).resolve().parents[2]


def sha256(path):
    digest = hashlib.sha256()
    with path.open("rb") as source:
        while block := source.read(8 * 1024**2):
            digest.update(block)
    return digest.hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source-image", type=Path, required=True, help="Original .img.bz2")
    parser.add_argument("--root", type=Path, required=True, help="Disposable fakefs root")
    parser.add_argument("--runtime", type=Path, default=REPO / "build/steamos-host/ish")
    parser.add_argument("--exporter", type=Path, default=REPO / "build/steamos-host/tools/unfakefsify")
    parser.add_argument("--cache", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True, help="New package directory")
    parser.add_argument("--app-manifest", type=Path, required=True)
    args = parser.parse_args()
    if sha256(args.source_image) != SOURCE_PIN:
        parser.error("Source image differs from the supplied SteamFrame image")
    if not (args.root / "meta.db").is_file():
        parser.error("--root must be an imported fakefs filesystem")
    args.output.mkdir(parents=True, exist_ok=False)
    args.cache.mkdir(parents=True, exist_ok=True)
    spec = importlib.util.spec_from_file_location("alarm", REPO / "tools/arch/alarm-bootstrap.py")
    alarm = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(alarm)
    support = REPO / "app/LinuxRunner/GUISupport.bundle"
    made = set()
    with tarfile.open(args.output / "overlay.tar", "w", format=tarfile.GNU_FORMAT) as overlay:
        def add(name, data, mode=0o644):
            alarm.add_bytes(overlay, name, data, made, mode)

        for name, expected in PACKAGES.items():
            package = args.cache / name
            if not package.exists():
                request = "https://fl.us.mirror.archlinuxarm.org/aarch64/extra/" + name
                with urllib.request.urlopen(request, timeout=120) as source, package.open("xb") as destination:
                    while block := source.read(1024**2):
                        destination.write(block)
            if sha256(package) != expected:
                raise ValueError("Package checksum mismatch: " + name)
            with tarfile.open(package) as archive:
                fields = {}
                for line in archive.extractfile(".PKGINFO").read().decode().splitlines():
                    if " = " in line:
                        key, value = line.split(" = ", 1)
                        fields.setdefault(key, []).append(value)
                files = []
                for member in archive:
                    if member.name.startswith("."):
                        continue
                    if ".." in member.name.split("/") or member.name.startswith("/"):
                        raise ValueError("Invalid package member")
                    files.append(member.name + ("/" if member.isdir() else ""))
                    if member.isdir():
                        alarm.add_dir(overlay, member.name.rstrip("/"), made)
                    else:
                        overlay.addfile(member, archive.extractfile(member) if member.isfile() else None)
                mapping = {"pkgname": "NAME", "pkgver": "VERSION", "pkgdesc": "DESC", "arch": "ARCH", "url": "URL", "license": "LICENSE", "depend": "DEPENDS", "optdepend": "OPTDEPENDS", "provides": "PROVIDES", "builddate": "BUILDDATE", "packager": "PACKAGER", "size": "ISIZE"}
                description = {mapping[key]: value for key, value in fields.items() if key in mapping}
                local = "usr/lib/holo/pacmandb/local/" + fields["pkgname"][0] + "-" + fields["pkgver"][0]
                add(local + "/desc", alarm.local_desc(description, True).encode())
                add(local + "/files", ("%FILES%\n" + "\n".join(files) + "\n\n").encode())
        for helper in HELPERS:
            add("usr/lib/linuxrunner/prebuilt-gui/" + helper + ".c", (support / (helper + ".c")).read_bytes())
        sources = "".join(sha256(support / (name + ".c")) + "  " + name + ".c\n" for name in HELPERS)
        add("usr/lib/linuxrunner/prebuilt-gui/sources.sha256", sources.encode())
        init = b'#!/bin/sh\nset -eu\nmkdir -p /tmp /run /var/log /var/lib/linuxrunner /home/mobile/Downloads\nchmod 1777 /tmp\ndbus-uuidgen --ensure\ntouch /var/lib/linuxrunner/initialized\nwhile :; do sleep 3600; done\n'
        add("usr/lib/linuxrunner/init.sh", init, 0o755)
    environment = dict(os.environ, ISH_BIND_MOUNTS="/mnt=" + str(args.output.resolve()))
    script = """set -eu
. /etc/os-release
[ "$ID" = steamos ] && [ "$(uname -m)" = aarch64 ]
tar -xf /mnt/overlay.tar -C /
x11vnc -version
pkg-config --exists x11 xcb
cd /usr/lib/linuxrunner/prebuilt-gui
sha256sum -c sources.sha256
for helper in gui-test focus-manager desktop-probe window-observer; do
 gcc "$helper.c" -O2 -Wall -Wextra -lX11 -o "$helper"
 chmod 755 "$helper"
done
sha256sum gui-test focus-manager desktop-probe window-observer > binaries.sha256
mkdir -p /var/lib/linuxrunner
touch /var/lib/linuxrunner/initialized
pacman -Q x11vnc libvncserver
echo STEAMOS_PREPARED=PASS
"""
    with (args.output / "prepare.log").open("w") as log:
        subprocess.run([str(args.runtime.resolve()), "-f", str(args.root.resolve()), "/bin/bash", "--noprofile", "--norc", "-ec", script], env=environment, stdout=log, stderr=subprocess.STDOUT, check=True, timeout=900)
    print("Guest helpers built; exporting prepared userspace", flush=True)
    archive = args.output / "rootfs.tar.gz"
    # fakefsify chooses export mode from argv[0]; preserve the unfakefsify alias.
    subprocess.run([str(args.exporter.absolute()), str(args.root.resolve()), str(archive.resolve())], check=True, timeout=900)
    installed = entries = 0
    with tarfile.open(archive) as image:
        for member in image:
            entries += 1
            if member.isfile():
                installed += member.size
    manifest = {"schema": 1, "architecture": "arm64", "version": "0.3.0", "sourceImageSHA256": SOURCE_PIN,
                "archiveSHA256": sha256(archive), "archiveBytes": archive.stat().st_size, "installedBytes": installed, "entries": entries}
    text = json.dumps(manifest, indent=2) + "\n"
    (args.output / "manifest.json").write_text(text)
    args.app_manifest.parent.mkdir(parents=True, exist_ok=True)
    args.app_manifest.write_text(text)
    print(text)


if __name__ == "__main__":
    main()
