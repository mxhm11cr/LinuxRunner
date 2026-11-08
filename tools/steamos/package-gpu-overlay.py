#!/usr/bin/env python3
"""Package the isolated SteamOS Mesa build without replacing system libraries.

--stage is the Mesa DESTDIR prefix (ending in opt/linuxrunner/steamos-gpu).
The archive is installable into existing environments; no rootfs reimport.
"""
import argparse
import gzip
import hashlib
import io
import json
from pathlib import Path
import posixpath
import shutil
import subprocess
import tarfile
import tempfile

MESA_REV = '590bf21d918c86908d96d1f4590ecd25b9657171'
SOURCE_PIN = '3a4a077f1b1f40688ab3279affcb56776bd97c54db1573e7c65fc52a97106676'
REPO = Path(__file__).resolve().parents[2]


def digest(path):
    h = hashlib.sha256()
    with path.open('rb') as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b''):
            h.update(block)
    return h.hexdigest()


def probe_source_bytes():
    """Preferred source may come from the separate host GPU PR or this archive."""
    source = REPO / 'tests/linuxrunner/gpu/virgl_border_contract.c'
    if source.is_file():
        return source.read_bytes()
    bundle = REPO / 'app/LinuxRunner/GUISupport.bundle'
    archive = bundle / 'steamos-gpu.tar.gz'
    expected = (bundle / 'steamos-gpu.sha256').read_text().split()[0]
    if digest(archive) != expected:
        raise ValueError('Bundled overlay checksum mismatch; cannot recover preferred probe source')
    with tarfile.open(archive, 'r:gz') as tar:
        name = 'steamos-gpu/share/linuxrunner/virgl_border_contract.c'
        member = tar.getmember(name)
        if not member.isfile() or member.size > 1024 * 1024:
            raise ValueError('Invalid bundled preferred probe source')
        data = tar.extractfile(member).read()
        build = json.load(tar.extractfile('steamos-gpu/BUILD.json'))
        if hashlib.sha256(data).hexdigest() != build['probeSourceSHA256']:
            raise ValueError('Bundled probe source identity mismatch')
        return data


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--stage', type=Path, required=True)
    p.add_argument('--mesa-source', type=Path, required=True)
    p.add_argument('--probe', type=Path, required=True, help='AArch64 virgl_border_contract executable')
    p.add_argument('--output', type=Path, required=True, help='New output directory')
    p.add_argument('--bundle', action='store_true', help='Stage archive in the app GUI support bundle')
    a = p.parse_args()
    revision = subprocess.check_output(['git', '-C', str(a.mesa_source), 'rev-parse', 'HEAD'], text=True).strip()
    if revision != MESA_REV or subprocess.check_output(['git', '-C', str(a.mesa_source), 'diff', 'HEAD', '--']):
        p.error('Use the pinned, unmodified Mesa source')
    stage = a.stage.resolve(strict=True)
    for required in ['lib/libgbm.so.1', 'lib/libEGL_mesa.so.0', 'lib/libGLX_mesa.so.0', 'lib/dri/virtio_gpu_dri.so']:
        if not (stage / required).is_file():
            p.error('Missing staged driver: ' + required)
    a.output.mkdir(parents=True, exist_ok=False)
    with tempfile.TemporaryDirectory(prefix='steamos-gpu-') as temporary:
        prefix = Path(temporary) / 'steamos-gpu'
        prefix.mkdir()
        for directory in ['lib', 'share']:
            shutil.copytree(stage / directory, prefix / directory, symlinks=True)
        notices = prefix / 'share/licenses/mesa'
        notices.mkdir(parents=True)
        shutil.copy2(a.mesa_source / 'docs/license.rst', notices / 'license.rst')
        shutil.copytree(a.mesa_source / 'licenses', notices / 'licenses')
        (prefix / 'bin').mkdir()
        shutil.copy2(a.probe, prefix / 'bin/graphics-check')
        (prefix / 'bin/graphics-check').chmod(0o755)
        probe_sources = prefix / 'share/linuxrunner'
        probe_sources.mkdir()
        probe_source = probe_source_bytes()
        (probe_sources / 'virgl_border_contract.c').write_bytes(probe_source)
        for name in ['LICENSE.md', 'LICENSE.IOS']:
            shutil.copy2(REPO / name, probe_sources / name)
        provenance = {
            'schema': 1, 'architecture': 'aarch64', 'sourceImageSHA256': SOURCE_PIN,
            'mesa': {'revision': MESA_REV, 'source': 'https://gitlab.freedesktop.org/mesa/mesa.git'},
            'prefix': '/opt/linuxrunner/steamos-gpu', 'glibc': '2.39',
            'probeSourceSHA256': hashlib.sha256(probe_source).hexdigest(),
            'options': {'gallium-drivers': 'virgl', 'vulkan-drivers': '', 'llvm': 'disabled',
                        'platforms': 'x11,wayland', 'glx': 'dri', 'glvnd': 'enabled', 'egl': 'enabled', 'gbm': 'enabled'},
        }
        (prefix / 'BUILD.json').write_text(json.dumps(provenance, indent=2) + '\n')
        files = sorted(prefix.rglob('*'))
        hashes = []
        for entry in files:
            name = entry.relative_to(prefix).as_posix()
            if entry.is_symlink():
                target = entry.readlink().as_posix()
                resolved = posixpath.normpath(posixpath.join(posixpath.dirname(name), target))
                if target.startswith('/') or resolved == '..' or resolved.startswith('../'):
                    p.error('Driver symlink escapes its isolated prefix: ' + name)
            elif entry.is_file():
                with entry.open('rb') as stream:
                    header = stream.read(20)
                if header.startswith(b'\x7fELF') and (header[4:6] != b'\x02\x01' or header[18:20] != b'\xb7\x00'):
                    p.error('Driver contains a non-AArch64 ELF: ' + name)
                hashes.append(digest(entry) + '  ' + name + '\n')
            elif not entry.is_dir():
                p.error('Unsupported driver entry: ' + name)
        (prefix / 'payload.sha256').write_text(''.join(hashes))
        archive = a.output / 'steamos-gpu.tar.gz'
        with archive.open('xb') as raw, gzip.GzipFile(fileobj=raw, mode='wb', filename='', mtime=0) as compressed:
            with tarfile.open(fileobj=compressed, mode='w', format=tarfile.GNU_FORMAT, dereference=False) as tar:
                for entry in [prefix] + sorted(prefix.rglob('*')):
                    info = tar.gettarinfo(str(entry), str(entry.relative_to(prefix.parent)))
                    info.uid = info.gid = info.mtime = 0
                    info.uname = info.gname = ''
                    info.mode = 0o755 if info.isdir() or (info.isfile() and entry.stat().st_mode & 0o111) else 0o644
                    with entry.open('rb') if info.isfile() else io.BytesIO() as data:
                        tar.addfile(info, data if info.isfile() else None)
        sums = a.output / 'steamos-gpu.sha256'
        sums.write_text(digest(archive) + '  steamos-gpu.tar.gz\n')
        (a.output / 'BUILD.json').write_text(json.dumps(provenance, indent=2) + '\n')
        if a.bundle:
            bundle = REPO / 'app/LinuxRunner/GUISupport.bundle'
            for entry in [archive, sums]:
                shutil.copy2(entry, bundle / entry.name)
            subprocess.run(['python3', str(REPO / 'scripts/generate-gui-support-manifest.py')], check=True)
        print(sums.read_text(), end='')


if __name__ == '__main__':
    main()
