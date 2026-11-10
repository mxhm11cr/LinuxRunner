#!/usr/bin/env python3
"""Inspect a local SteamFrame image and export experimental Linux userspace.

Does not authenticate Valve images, boot a kernel, or install an app environment.
Requires Python 3.10–3.14 and requirements.txt from this directory.
"""

import argparse
import bz2
import contextlib
import json
import os
from pathlib import Path
import struct
import sys
import tarfile
import tempfile
import uuid
import zlib


GIB = 1024**3
EXCLUDED = {
    "boot", "efi", "esp", "persist", "usr/lib/modules", "usr/lib/firmware", "usr/lib/debug",
}
EMPTY_DIRECTORIES = {"dev", "proc", "sys", "run", "tmp"}
PROBES = ("bash", "pacman", "Xvfb", "kwin_x11", "plasmashell", "startplasma-x11")


@contextlib.contextmanager
def open_image(path, limit):
    with path.open("rb") as source:
        compressed = source.read(3) == b"BZh"
        source.seek(0)
        if not compressed:
            if os.fstat(source.fileno()).st_size > limit:
                raise ValueError("Image exceeds --max-image-gib")
            yield source
            return
        # Random access is needed for Btrfs. The source is never modified.
        with tempfile.TemporaryFile() as unpacked, bz2.BZ2File(source) as reader:
            size = 0
            while block := reader.read(8 * 1024**2):
                size += len(block)
                if size > limit:
                    raise ValueError("Decompressed image exceeds --max-image-gib")
                unpacked.write(block)
            unpacked.seek(0)
            yield unpacked


def partitions(image):
    image.seek(0, os.SEEK_END)
    image_size = image.tell()
    for sector in (512, 4096):
        image.seek(sector)
        header = image.read(sector)
        if header[:8] == b"EFI PART":
            break
    else:
        raise ValueError("No GPT partition table found")
    header_size, checksum = struct.unpack_from("<II", header, 12)
    if not 92 <= header_size <= sector:
        raise ValueError("Invalid GPT header size")
    check = bytearray(header[:header_size])
    check[16:20] = b"\0" * 4
    if zlib.crc32(check) != checksum:
        raise ValueError("GPT header checksum mismatch")
    table_lba, count, entry_size, table_checksum = struct.unpack_from("<QIII", header, 72)
    table_size = count * entry_size
    if not count or entry_size < 128 or entry_size % 128 or table_size > 1024**2:
        raise ValueError("Invalid GPT entry table size")
    if table_lba * sector + table_size > image_size:
        raise ValueError("GPT entry table exceeds image")
    image.seek(table_lba * sector)
    table = image.read(table_size)
    if zlib.crc32(table) != table_checksum:
        raise ValueError("GPT entry table checksum mismatch")
    result = []
    for index in range(count):
        entry = table[index * entry_size:(index + 1) * entry_size]
        if entry[:16] == b"\0" * 16:
            continue
        first, last = struct.unpack_from("<QQ", entry, 32)
        if first > last or (last + 1) * sector > image_size:
            raise ValueError("Partition exceeds image")
        result.append({
            "number": index + 1,
            "name": entry[56:128].decode("utf-16-le").rstrip("\0"),
            "type_guid": str(uuid.UUID(bytes_le=entry[:16])),
            "offset": first * sector,
            "size": (last - first + 1) * sector,
        })
    return result


def resolve(fs, path):
    """Resolve guest symlinks without following any host filesystem path."""
    import posixpath

    for _ in range(40):
        parts = posixpath.normpath(path).strip("/").split("/")
        for index in range(len(parts)):
            current = "/" + "/".join(parts[:index + 1])
            node = fs.get(current)
            if node.is_symlink():
                link = node.link
                if not link.startswith("/"):
                    link = posixpath.join(posixpath.dirname(current), link)
                path = posixpath.join(link, *parts[index + 1:])
                break
        else:
            return node
    raise ValueError("Guest symlink loop: " + path)


def inspect(fs, table):
    with resolve(fs, "/etc/os-release").open() as source:
        release = source.read(65536).decode()
    fields = {}
    for line in release.splitlines():
        if "=" in line:
            key, value = line.split("=", 1)
            fields[key] = value.strip('"')
    if fields.get("ID") != "steamos":
        raise ValueError("The selected root is not identified as SteamOS")
    binaries = {}
    for name in PROBES:
        try:
            with resolve(fs, "/usr/bin/" + name).open() as binary:
                header = binary.read(64)
        except FileNotFoundError:
            binaries[name] = {"present": False}
            continue
        if len(header) < 20 or header[:6] != b"\x7fELF\x02\x01":
            raise ValueError("Expected a little-endian ELF64 binary: " + name)
        machine = struct.unpack_from("<H", header, 18)[0]
        if machine != 183:
            raise ValueError("Expected ARM64 ELF machine 183: " + name)
        binaries[name] = {"present": True, "elf_machine": machine}
    if not binaries["bash"]["present"]:
        raise ValueError("ARM64 Bash is required for this userspace experiment")
    # Deliberately avoid logging package repository URLs or account files.
    return {"os_release": fields, "partitions": table, "binaries": binaries,
            "excluded_paths": sorted(EXCLUDED), "status": "experimental userspace; not app support"}


def export(fs, output):
    inodes = {}
    count = total = 0
    with tarfile.open(fileobj=output, mode="w|", format=tarfile.GNU_FORMAT) as archive:
        def add(path, node):
            nonlocal count, total
            if path in EXCLUDED:
                return
            info = tarfile.TarInfo(path)
            info.mode = node.mode & 0o7777
            info.uid, info.gid = node.uid, node.gid
            info.mtime = int(node.mtime.timestamp())
            if node.is_dir():
                info.type = tarfile.DIRTYPE
                archive.addfile(info)
                if path not in EMPTY_DIRECTORIES:
                    for name, child in sorted(node.listdir().items()):
                        if name in {".", ".."}:
                            continue
                        if not name or "/" in name or "\0" in name:
                            raise ValueError("Invalid directory entry in image")
                        add(path + "/" + name, child)
            elif node.is_symlink():
                info.type, info.linkname = tarfile.SYMTYPE, node.link
                archive.addfile(info)
            elif node.is_file():
                identity = (node.subvolume.objectid, node.inum)
                if identity in inodes:
                    info.type, info.linkname = tarfile.LNKTYPE, inodes[identity]
                    archive.addfile(info)
                else:
                    info.size = node.size
                    with node.open() as data:
                        archive.addfile(info, data)
                    inodes[identity] = path
                    total += info.size
            else:
                return  # Never materialize device nodes or sockets on the host.
            count += 1

        for name, node in sorted(fs.root.listdir().items()):
            if name not in {".", ".."}:
                if not name or "/" in name or "\0" in name:
                    raise ValueError("Invalid root directory entry in image")
                add(name, node)
    return {"entries": count, "file_bytes": total}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("image", type=Path, help="Local .img or .img.bz2")
    parser.add_argument("--output", type=Path, help="New, uncompressed userspace .tar; omit to inspect")
    parser.add_argument("--report", type=Path, help="New JSON inspection report")
    parser.add_argument("--max-image-gib", type=int, default=16)
    args = parser.parse_args()
    if not (3, 10) <= sys.version_info[:2] < (3, 15):
        parser.error("Use Python 3.10–3.14 with the pinned Btrfs parser")
    if args.max_image_gib < 1:
        parser.error("--max-image-gib must be positive")
    for target in (args.output, args.report):
        if target and (target.exists() or target.is_symlink()):
            parser.error("Refusing to overwrite " + str(target))
    from dissect.btrfs import Btrfs
    from dissect.util.stream import RangeStream

    with open_image(args.image, args.max_image_gib * GIB) as image:
        table = partitions(image)
        roots = [p for p in table if p["name"] == "rootfs-A"]
        if len(roots) != 1:
            raise ValueError("Expected exactly one rootfs-A partition")
        root = roots[0]
        fs = Btrfs(RangeStream(image, root["offset"], root["size"]))
        report = inspect(fs, table)
        if args.output:
            # Publish a completed archive exclusively; leave no partial result on failure.
            with tempfile.NamedTemporaryFile(dir=args.output.parent) as staged:
                report["export"] = export(fs, staged)
                staged.flush()
                os.link(staged.name, args.output)
        text = json.dumps(report, indent=2) + "\n"
        if args.report:
            with args.report.open("x") as destination:
                destination.write(text)
        print(text, end="")


if __name__ == "__main__":
    try:
        main()
    except (ValueError, OSError) as error:
        sys.exit(str(error))
