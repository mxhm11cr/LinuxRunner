#!/usr/bin/env python3
"""Extract the root Btrfs partition from Valve's pinned Steam Frame ZIP.

Developer qualification tool, not a supported SteamOS installer. Streams the
ZIP entry, checks its complete CRC/length and GPT CRCs, and writes only the root
partition (sparsely). Does not mount/boot an image, invoke x86 translation, alter
an existing rootfs, or claim that Steam/client/gaming sessions work.
"""
import argparse
import hashlib
import io
import json
from pathlib import Path
import shutil
import struct
import time
import urllib.request
import zipfile
import zlib

URL = 'https://steamdeck-images.steamos.cloud/recovery/steamframe-oobe-repair-20260922.5153644-0.3.0.img.zip'
MIB = 1024 * 1024


class Metadata(io.RawIOBase):
    def __init__(self):
        with urllib.request.urlopen(urllib.request.Request(URL, method='HEAD'), timeout=30) as r:
            self.size = int(r.headers['Content-Length'])
            self.etag = r.headers.get('ETag')
        self.pos = self.total = 0

    def seekable(self): return True
    def readable(self): return True
    def tell(self): return self.pos

    def seek(self, offset, whence=0):
        self.pos = offset if whence == 0 else self.pos + offset if whence == 1 else self.size + offset
        if self.pos < 0: raise ValueError('negative offset')
        return self.pos

    def read(self, n=-1):
        n = min(self.size - self.pos, n if n >= 0 else self.size - self.pos)
        if n <= 0: return b''
        if n > MIB or self.total + n > 2 * MIB: raise ValueError('ZIP metadata budget exceeded')
        headers = {'Range': f'bytes={self.pos}-{self.pos+n-1}'}
        if self.etag: headers['If-Match'] = self.etag
        with urllib.request.urlopen(urllib.request.Request(URL, headers=headers), timeout=30) as r:
            if r.status != 206: raise ValueError('server ignored metadata range')
            data = r.read(n + 1)
            if len(data) != n: raise ValueError('invalid metadata response')
        self.pos += n
        self.total += n
        return data


def root_partition(prefix, image_size):
    sector = next((s for s in (512, 4096) if prefix[s:s+8] == b'EFI PART'), None)
    if not sector: raise ValueError('no GPT signature')
    header = prefix[sector:sector*2]
    size, expected = struct.unpack_from('<II', header, 12)
    if not 92 <= size <= sector: raise ValueError('invalid GPT header size')
    checked = bytearray(header[:size]); checked[16:20] = b'\0' * 4
    if zlib.crc32(checked) != expected: raise ValueError('GPT header CRC mismatch')
    lba = struct.unpack_from('<Q', header, 72)[0]
    count, entry_size, crc = struct.unpack_from('<III', header, 80)
    if count > 1024 or not 128 <= entry_size <= 4096: raise ValueError('invalid GPT entries')
    entries = prefix[lba*sector:lba*sector+count*entry_size]
    if len(entries) != count*entry_size or zlib.crc32(entries) != crc:
        raise ValueError('GPT partition array unavailable or CRC mismatch')
    matches = []
    for i in range(count):
        entry = entries[i*entry_size:(i+1)*entry_size]
        if not any(entry[:16]): continue
        name = entry[56:128].decode('utf-16le').rstrip('\0')
        first, last = struct.unpack_from('<QQ', entry, 32)
        start, length = first*sector, (last-first+1)*sector
        if first > last or start < len(prefix) or start + length > image_size:
            # Small ESP partitions may start inside this prefix; only the root
            # partition is extracted and needs the stricter lower bound.
            if name == 'rootfs-A': raise ValueError('invalid root partition bounds')
        if name == 'rootfs-A': matches.append((start, length))
    if len(matches) != 1 or matches[0][1] > 6*1024*MIB:
        raise ValueError('missing, ambiguous or oversized root partition')
    return matches[0]


def extract(output):
    partial = output.with_name(output.name + '.partial')
    report = output.with_suffix(output.suffix + '.json')
    if output.exists() or partial.exists() or report.exists():
        raise ValueError('output/evidence already exists; choose a new destination')
    output.parent.mkdir(parents=True, exist_ok=True)
    if shutil.disk_usage(output.parent).free < 8 * 1024 * MIB:
        raise ValueError('need at least 8 GiB free for extraction and host reserve')
    remote = Metadata()
    with zipfile.ZipFile(remote) as archive:
        entries = archive.infolist()
        if len(entries) != 1: raise ValueError('unexpected recovery archive layout')
        entry = entries[0]
    if entry.filename != 'lun0.img' or entry.compress_type != zipfile.ZIP_DEFLATED or entry.flag_bits & 1:
        raise ValueError('unexpected or encrypted recovery image')
    if entry.file_size > 8*1024*MIB or entry.compress_size > 5*1024*MIB:
        raise ValueError('image exceeds bounded extraction limits')
    remote.seek(entry.header_offset)
    local = remote.read(30)
    if local[:4] != b'PK\x03\x04': raise ValueError('invalid local ZIP header')
    name_size, extra_size = struct.unpack_from('<HH', local, 26)
    begin = entry.header_offset + 30 + name_size + extra_size
    end = begin + entry.compress_size - 1
    headers = {'Range': f'bytes={begin}-{end}'}
    if remote.etag: headers['If-Match'] = remote.etag
    decoder = zlib.decompressobj(-15)
    raw_size = received = crc = 0
    prefix = bytearray(); bounds = None
    image_hash = hashlib.sha256(); root_hash = hashlib.sha256()
    last_progress = time.monotonic()
    # Keep a failed .partial for diagnosis; never promote an unverified image.
    with partial.open('xb') as target, urllib.request.urlopen(urllib.request.Request(URL, headers=headers), timeout=60) as response:
        partial.chmod(0o600)
        if response.status != 206: raise ValueError('server ignored payload range')
        while received < entry.compress_size:
            data = response.read(min(MIB, entry.compress_size-received))
            if not data: raise ValueError('truncated ZIP payload')
            received += len(data)
            while data:
                decoded = decoder.decompress(data, MIB)
                data = decoder.unconsumed_tail
                if not decoded: continue
                crc = zlib.crc32(decoded, crc); image_hash.update(decoded)
                if len(prefix) < 65536:
                    prefix.extend(decoded[:65536-len(prefix)])
                    if len(prefix) == 65536: bounds = root_partition(prefix, entry.file_size)
                if bounds:
                    start, length = bounds
                    lo, hi = max(0, start-raw_size), min(len(decoded), start+length-raw_size)
                    if hi > lo:
                        piece = decoded[lo:hi]; root_hash.update(piece)
                        target.seek(raw_size+lo-start)
                        if any(piece): target.write(piece)
                raw_size += len(decoded)
                if raw_size > entry.file_size: raise ValueError('expanded image exceeds declared size')
            if time.monotonic()-last_progress >= 10:
                print(f'SteamOS recovery: {received*100/entry.compress_size:.1f}% compressed payload read', flush=True)
                last_progress = time.monotonic()
        if not decoder.eof or decoder.unused_data or raw_size != entry.file_size or crc != entry.CRC or not bounds:
            raise ValueError('complete ZIP image CRC/size validation failed')
        target.truncate(bounds[1]); target.flush()
        import os
        os.fsync(target.fileno())
    with partial.open('rb') as f:
        f.seek(65536+64)
        if f.read(8) != b'_BHRfS_M': raise ValueError('root partition is not the expected Btrfs filesystem')
    output.hardlink_to(partial)  # fail rather than overwrite an existing output
    partial.unlink()
    result = dict(source=URL,etag=remote.etag,zip_entry=entry.filename,zip_crc32=f'{crc:08x}',
                  complete_image_crc_verified=True,image_sha256=image_hash.hexdigest(),
                  compressed_bytes=received,image_bytes=raw_size,root_partition_offset=bounds[0],
                  root_partition_bytes=bounds[1],root_partition_sha256=root_hash.hexdigest(),
                  official_signed_checksum_verified=False,guest_execution_tested=False)
    with report.open('x') as f: f.write(json.dumps(result, indent=2)+'\n')
    print(json.dumps(result, indent=2), flush=True)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, required=True, help='New host Btrfs image path (never a rootfs directory)')
    args = parser.parse_args()
    extract(args.output)
