#!/usr/bin/env python3
"""Prepare a genuine, minimal SteamOS ARM64 shell archive for developer tests.

Not a complete SteamOS installer. Uses an explicitly supplied LinuxRunner CLI
and disposable helper rootfs. Never runs x86 code or patches installed programs.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import struct
import subprocess
import tarfile
import urllib.request

TOOL_URL = 'https://github.com/kdave/btrfs-progs/releases/download/v7.3/btrfs.static-arm'
TOOL_SHA256 = '2bb4f448c05510ab034e7d89c12f904eb8e33424f303e628dc0648c5d06040a1'
IMAGE_SHA256 = '668da2b1d32e9b779a1ee27ed570961ceaaf83a5dfeadd8129b62603223c319d'
# Include ancestor directories: btrfs restore applies its filter to each level.
PROFILE = r'^/(|usr(|/bin(|/(bash|env|id|uname|printf|cat))|/lib(|/(ld-linux.*|ld-[0-9].*|libc[.-].*|libdl[.-].*|libpthread[.-].*|libm[.-].*|libtinfo[.-].*|libncurses.*|libreadline[.-].*|libcap[.-].*|libattr[.-].*|libnss_files[.-].*|libnss_dns[.-].*|libresolv[.-].*|os-release)))|etc(|/os-release)|bin|lib|lib64|dev|proc|sys|run|tmp|root|home)$'


def sha256(path):
    with path.open('rb') as f: return hashlib.file_digest(f, 'sha256').hexdigest()


def arm64(header):
    return len(header) >= 20 and header[:6] == b'\x7fELF\x02\x01' and struct.unpack_from('<H', header, 18)[0] == 183


def verified_tool(work):
    target = work / 'btrfs-7.3-aarch64'
    request = urllib.request.Request(TOOL_URL, headers={'User-Agent': 'LinuxRunner-developer-validation'})
    partial = target.with_suffix('.partial')
    with urllib.request.urlopen(request, timeout=60) as response, partial.open('xb') as output:
        count = 0
        while data := response.read(1024*1024):
            count += len(data)
            if count > 16*1024*1024: raise ValueError('helper download exceeds size limit')
            output.write(data)
    if sha256(partial) != TOOL_SHA256: raise ValueError('upstream helper SHA-256 mismatch')
    with partial.open('rb') as f:
        if not arm64(f.read(64)): raise ValueError('helper is not AArch64 ELF64')
    target.hardlink_to(partial); partial.unlink()
    return target


def validate_archive(path):
    required = {'usr/bin/bash': False, 'usr/lib/os-release': False}
    with tarfile.open(path, 'r:') as archive:
        for member in archive:
            name = member.name.removeprefix('./').rstrip('/')
            if name == '.': continue
            parts = name.split('/')
            if name.startswith('/') or '..' in parts or '\0' in name:
                raise ValueError('unsafe archive member')
            if name == 'usr/bin/bash':
                if not member.isfile() or not arm64(archive.extractfile(member).read(64)):
                    raise ValueError('restored shell is not an AArch64 ELF64 executable')
                required[name] = True
            if name == 'usr/lib/os-release':
                if not member.isfile(): raise ValueError('missing real distribution identity')
                identity = archive.extractfile(member).read(65537)
                if len(identity) > 65536 or b'ID=steamos' not in identity.splitlines():
                    raise ValueError('archive does not contain SteamOS identity')
                required[name] = True
    if not all(required.values()): raise ValueError('required SteamOS files missing')


def prepare(runtime, helper, image, work, output):
    runtime, helper, image = (p.resolve(strict=True) for p in (runtime, helper, image))
    work, output = work.absolute(), output.absolute()
    manifest = output.with_suffix(output.suffix+'.json')
    if work.exists() or output.exists() or manifest.exists():
        raise ValueError('work/output already exists; choose new paths')
    if any(',' in str(p) or '\n' in str(p) for p in (work, image.parent)):
        raise ValueError('CLI bind paths must not contain commas or newlines')
    if sha256(image) != IMAGE_SHA256: raise ValueError('wrong or incomplete recovery root partition')
    work.mkdir(parents=True)
    if shutil.disk_usage(work).free < 256*1024*1024: raise ValueError('insufficient staging space')
    tool = verified_tool(work)
    env = dict(os.environ, ISH_BIND_MOUNTS=f'/home/mobile/Downloads={work},/mnt/steam-recovery={image.parent}:ro')
    # Data and regex are argv, never interpolated shell source. -m/-S retain
    # source modes/owners/times/symlinks; this profile makes no xattr/caps claim.
    command = '''set -eu
cd /home/mobile/Downloads
chmod 755 "$1"
"./$1" version
mkdir files
"./$1" restore -m -S --path-regex "$2" "/mnt/steam-recovery/$3" /home/mobile/Downloads/files
tar -cpf shell.tar -C files .
'''
    with (work/'restore.log').open('w') as log:
        subprocess.run([str(runtime), '-f', str(helper), '/bin/sh', '-ec', command,
                        'linuxrunner-steamos-prepare', tool.name, PROFILE, image.name],
                       env=env, stdout=log, stderr=subprocess.STDOUT, timeout=240, check=True)
    archive = work/'shell.tar'
    validate_archive(archive)
    output.parent.mkdir(parents=True, exist_ok=True)
    with archive.open('rb') as source, output.open('xb') as target:
        shutil.copyfileobj(source, target, 1024*1024); target.flush(); os.fsync(target.fileno())
    result = dict(profile='minimal-shell', distribution='SteamOS', architecture='aarch64',
                  image_sha256=IMAGE_SHA256, helper_source=TOOL_URL, helper_sha256=TOOL_SHA256,
                  archive_sha256=sha256(output), archive_bytes=output.stat().st_size,
                  desktop_verified=False, package_management_verified=False,
                  full_metadata_verified=False, steam_client_verified=False,
                  reason='Qualification archive only; not complete SteamOS support')
    with manifest.open('x') as f: json.dump(result, f, indent=2); f.write('\n')
    return result


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    for key in ('runtime', 'helper-rootfs', 'image', 'work', 'output'):
        parser.add_argument('--'+key, type=Path, required=True)
    args = parser.parse_args()
    print(json.dumps(prepare(args.runtime, args.helper_rootfs, args.image, args.work, args.output), indent=2))
