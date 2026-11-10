#!/usr/bin/env python3
"""Offline validation contracts; real SteamOS execution is tested separately."""
import importlib.util
import io
from pathlib import Path
import struct
import tarfile
import tempfile
import unittest

spec = importlib.util.spec_from_file_location('prepare', Path(__file__).resolve().parents[2] / 'scripts/prepare-steamos-shell.py')
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)


def header(machine=183):
    data = bytearray(64); data[:6] = b'\x7fELF\x02\x01'; struct.pack_into('<H', data, 18, machine)
    return bytes(data)


class ArchiveValidation(unittest.TestCase):
    def validate(self, files):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / 'shell.tar'
            with tarfile.open(path, 'w') as archive:
                for name, data in files:
                    member = tarfile.TarInfo(name); member.size = len(data)
                    archive.addfile(member, io.BytesIO(data))
            module.validate_archive(path)

    def test_real_identity_and_arm64_required(self):
        self.validate([('./usr/bin/bash', header()), ('./usr/lib/os-release', b'ID=steamos\n')])

    def test_x86_rejected(self):
        with self.assertRaises(ValueError): self.validate([('usr/bin/bash', header(62)), ('usr/lib/os-release', b'ID=steamos\n')])

    def test_other_distribution_rejected(self):
        with self.assertRaises(ValueError): self.validate([('usr/bin/bash', header()), ('usr/lib/os-release', b'ID=arch\n')])

    def test_missing_identity_rejected(self):
        with self.assertRaises(ValueError): self.validate([('usr/bin/bash', header())])

    def test_path_traversal_rejected(self):
        with self.assertRaises(ValueError): self.validate([('../outside', b'x'), ('usr/bin/bash', header()), ('usr/lib/os-release', b'ID=steamos\n')])

    def test_absolute_path_rejected(self):
        with self.assertRaises(ValueError): self.validate([('/outside', b'x'), ('usr/bin/bash', header()), ('usr/lib/os-release', b'ID=steamos\n')])

    def test_short_binary_rejected(self):
        with self.assertRaises(ValueError): self.validate([('usr/bin/bash', b'ELF'), ('usr/lib/os-release', b'ID=steamos\n')])

    def test_large_identity_rejected(self):
        with self.assertRaises(ValueError): self.validate([('usr/bin/bash', header()), ('usr/lib/os-release', b'ID=steamos\n'+b'x'*65536)])


if __name__ == '__main__': unittest.main()
