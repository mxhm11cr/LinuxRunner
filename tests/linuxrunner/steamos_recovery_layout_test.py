#!/usr/bin/env python3
"""Offline bounds/CRC tests for the recovery partition extractor; no guest claims."""
import importlib.util
from pathlib import Path
import struct
import tempfile
import unittest
import zlib

spec = importlib.util.spec_from_file_location('recovery', Path(__file__).resolve().parents[2] / 'scripts/extract-steamos-recovery-rootfs.py')
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)


def layout(duplicate=False, start=335561728, size=5368709120):
    data = bytearray(65536)
    entry = bytearray(128)
    entry[:16] = bytes(range(16))
    struct.pack_into('<QQ', entry, 32, start//512, (start+size)//512-1)
    name = 'rootfs-A'.encode('utf-16le'); entry[56:56+len(name)] = name
    data[1024:1152] = entry
    if duplicate: data[1152:1280] = entry
    data[512:520] = b'EFI PART'
    struct.pack_into('<I', data, 524, 92)
    struct.pack_into('<QIII', data, 584, 2, 128, 128, zlib.crc32(data[1024:17408]))
    struct.pack_into('<I', data, 528, zlib.crc32(data[512:604]))
    return data


class RecoveryLayout(unittest.TestCase):
    def test_root_bounds(self):
        self.assertEqual(module.root_partition(layout(), 7516192768), (335561728, 5368709120))

    def test_header_crc(self):
        data = layout(); data[540] ^= 1
        with self.assertRaises(ValueError): module.root_partition(data, 7516192768)

    def test_partition_crc(self):
        data = layout(); data[1080] ^= 1
        with self.assertRaises(ValueError): module.root_partition(data, 7516192768)

    def test_duplicate_root(self):
        with self.assertRaises(ValueError): module.root_partition(layout(duplicate=True), 7516192768)

    def test_image_bounds(self):
        with self.assertRaises(ValueError): module.root_partition(layout(), 1024*1024*1024)

    def test_no_gpt(self):
        with self.assertRaises(ValueError): module.root_partition(bytes(65536), 7516192768)

    def test_existing_output_not_touched(self):
        with tempfile.TemporaryDirectory() as directory:
            out = Path(directory) / 'owned.img'; out.write_bytes(b'KEEP')
            with self.assertRaises(ValueError): module.extract(out)
            self.assertEqual(out.read_bytes(), b'KEEP')

    def test_existing_partial_not_touched(self):
        with tempfile.TemporaryDirectory() as directory:
            out = Path(directory) / 'owned.img'; partial = out.with_name(out.name+'.partial'); partial.write_bytes(b'KEEP')
            with self.assertRaises(ValueError): module.extract(out)
            self.assertEqual(partial.read_bytes(), b'KEEP')


if __name__ == '__main__': unittest.main()
