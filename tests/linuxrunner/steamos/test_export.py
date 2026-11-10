"""Offline contracts for the local SteamFrame image exporter."""

import bz2
from datetime import datetime, timezone
import importlib.util
import io
from pathlib import Path
import struct
import tarfile
import tempfile
import types
import unittest
import zlib


SCRIPT = Path(__file__).resolve().parents[3] / "tools/steamos/export-steamframe.py"
SPEC = importlib.util.spec_from_file_location("steamframe_export", SCRIPT)
exporter = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(exporter)


def disk_image():
    image = bytearray(64 * 512)
    table = bytearray(128)
    table[:16] = b"\x01" * 16
    struct.pack_into("<QQ", table, 32, 4, 50)
    name = "rootfs-A".encode("utf-16-le")
    table[56:56 + len(name)] = name
    image[1024:1152] = table
    header = bytearray(92)
    header[:8] = b"EFI PART"
    struct.pack_into("<I", header, 12, 92)
    struct.pack_into("<QIII", header, 72, 2, 1, 128, zlib.crc32(table))
    struct.pack_into("<I", header, 16, zlib.crc32(header))
    image[512:604] = header
    return image


class Node:
    def __init__(self, data=b"", *, children=None, link=None, inode=1):
        self.children, self.link, self.data = children, link, data
        self.inum = inode
        self.subvolume = types.SimpleNamespace(objectid=5)
        self.mode = 0o755 if children is not None else 0o640
        self.uid, self.gid = 1000, 100
        self.mtime = datetime(2026, 9, 22, tzinfo=timezone.utc)
        self.size = len(data)

    def is_dir(self): return self.children is not None
    def is_symlink(self): return self.link is not None
    def is_file(self): return not self.is_dir() and not self.is_symlink()
    def listdir(self): return self.children
    def open(self): return io.BytesIO(self.data)


class ExportTests(unittest.TestCase):
    def test_inspection_requires_arm64_even_when_partition_name_matches(self):
        binary = bytearray(64)
        binary[:6] = b"\x7fELF\x02\x01"
        struct.pack_into("<H", binary, 18, 62)  # x86-64, not ARM64
        root = Node(children={
            "etc": Node(children={"os-release": Node(b"ID=steamos\n")}),
            "usr": Node(children={"bin": Node(children={"bash": Node(binary)})}),
        })

        def get(path):
            node = root
            for name in path.strip("/").split("/"):
                try:
                    node = node.children[name]
                except KeyError:
                    raise FileNotFoundError(path) from None
            return node

        with self.assertRaisesRegex(ValueError, "Expected ARM64"):
            exporter.inspect(types.SimpleNamespace(get=get), [])

    def test_image_partitions_and_corrupt_metadata(self):
        image = disk_image()
        root, = exporter.partitions(io.BytesIO(image))
        self.assertEqual((root["name"], root["offset"], root["size"]),
                         ("rootfs-A", 2048, 47 * 512))
        image[1024] ^= 1
        with self.assertRaisesRegex(ValueError, "entry table checksum"):
            exporter.partitions(io.BytesIO(image))

    def test_reject_truncated_partition_and_corrupt_header(self):
        image = disk_image()
        with self.assertRaisesRegex(ValueError, "Partition exceeds"):
            exporter.partitions(io.BytesIO(image[:20 * 512]))
        image[520] ^= 1
        with self.assertRaisesRegex(ValueError, "header checksum"):
            exporter.partitions(io.BytesIO(image))

    def test_compressed_image_limit_and_source_preservation(self):
        with tempfile.TemporaryDirectory() as temporary:
            source = Path(temporary) / "image.bz2"
            compressed = bz2.compress(bytes(disk_image()))
            source.write_bytes(compressed)
            with exporter.open_image(source, 64 * 512) as image:
                self.assertEqual(image.read(), disk_image())
            with self.assertRaisesRegex(ValueError, "exceeds"):
                with exporter.open_image(source, 1024):
                    self.fail("Oversized image admitted")
            self.assertEqual(source.read_bytes(), compressed)

    def test_archive_preserves_guest_metadata_links_and_excludes_live_state(self):
        file = Node(b"SteamOS\n", inode=2)
        fs = types.SimpleNamespace(root=Node(children={
            "etc": Node(children={"os-release": file, "release-copy": file}),
            "bin": Node(link="usr/bin"),
            "tmp": Node(children={"private-session": Node(b"omit")}),
            "boot": Node(children={"kernel": Node(b"omit")}),
        }))
        buffer = io.BytesIO()
        result = exporter.export(fs, buffer)
        with tarfile.open(fileobj=io.BytesIO(buffer.getvalue())) as archive:
            self.assertEqual(archive.getnames(),
                             ["bin", "etc", "etc/os-release", "etc/release-copy", "tmp"])
            original = archive.getmember("etc/os-release")
            self.assertEqual((original.uid, original.gid, original.mode), (1000, 100, 0o640))
            self.assertTrue(archive.getmember("bin").issym())
            self.assertTrue(archive.getmember("etc/release-copy").islnk())
            self.assertEqual(archive.extractfile("etc/release-copy").read(), b"SteamOS\n")
        self.assertEqual(result["file_bytes"], len(b"SteamOS\n"))

    def test_archive_rejects_parent_path_entry(self):
        fs = types.SimpleNamespace(root=Node(children={"../escape": Node(b"bad")}))
        with self.assertRaisesRegex(ValueError, "Invalid root directory entry"):
            exporter.export(fs, io.BytesIO())


if __name__ == "__main__":
    unittest.main()
