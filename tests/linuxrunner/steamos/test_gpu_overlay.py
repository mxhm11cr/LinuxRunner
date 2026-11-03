"""Offline overlay source recovery and optional tags-workaround contracts."""
import hashlib
import importlib.util
from pathlib import Path
import shutil
import subprocess
import tarfile
import tempfile
import unittest

REPO = Path(__file__).resolve().parents[3]
BUNDLE = REPO / 'app/LinuxRunner/GUISupport.bundle'
SPEC = importlib.util.spec_from_file_location('gpu_packager', REPO / 'tools/steamos/package-gpu-overlay.py')
PACKAGER = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(PACKAGER)

class OverlayTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.root = Path(self.temp.name)
        self.previous = PACKAGER.REPO
        PACKAGER.REPO = self.root
        self.bundle = self.root / 'app/LinuxRunner/GUISupport.bundle'
        self.bundle.mkdir(parents=True)
        for name in ('steamos-gpu.tar.gz', 'steamos-gpu.sha256'):
            shutil.copy2(BUNDLE / name, self.bundle / name)

    def tearDown(self):
        PACKAGER.REPO = self.previous
        self.temp.cleanup()

    def test_clean_checkout_recovers_matching_preferred_source(self):
        with tarfile.open(self.bundle / 'steamos-gpu.tar.gz') as archive:
            source = archive.extractfile('steamos-gpu/share/linuxrunner/virgl_border_contract.c').read()
        self.assertEqual(PACKAGER.probe_source_bytes(), source)

    def test_modified_archive_is_refused(self):
        with (self.bundle / 'steamos-gpu.tar.gz').open('ab') as stream:
            stream.write(b'modified')
        with self.assertRaisesRegex(ValueError, 'checksum mismatch'):
            PACKAGER.probe_source_bytes()

    def test_standalone_preferred_source_is_used(self):
        source = self.root / 'tests/linuxrunner/gpu/virgl_border_contract.c'
        source.parent.mkdir(parents=True)
        source.write_bytes(b'preferred source')
        self.assertEqual(PACKAGER.probe_source_bytes(), b'preferred source')

    def test_tags_symlink_refusal_keeps_desktop_starting(self):
        # Relocate the actual workaround into a disposable host fixture. The
        # production script accepts no test root or filesystem overrides.
        plugin = self.root / 'usr/lib/qt6/plugins/kf6/kio/tags.so'
        plugin.parent.mkdir(parents=True)
        custom = self.root / 'custom-worker'
        custom.write_text('custom')
        plugin.symlink_to(custom)
        (self.root / 'os-release').write_text('ID=steamos\n')
        (self.root / 'passwd').write_text('mobile:x:1000:1000::' + str(self.root / 'home') + ':/bin/sh\n')
        script = (BUNDLE / 'steamos-desktop.sh').read_text()
        script = script.replace('$(id -u)', '0').replace('/etc/os-release', str(self.root / 'os-release'))
        script = script.replace('/etc/passwd', str(self.root / 'passwd'))
        script = script.replace('/usr/lib/qt6/plugins/kf6/kio/tags.so', str(plugin))
        script = script.replace('/opt', str(self.root / 'opt'))
        (self.root / 'steamos-desktop.sh').write_text(script)
        invocation = next(line.strip() for line in (BUNDLE / 'session.sh').read_text().splitlines()
                          if '/bin/sh ./steamos-desktop.sh tags-policy' in line)
        result = subprocess.run(['/bin/sh', '-ec', 'guest_user=mobile\n' + invocation + '\nprintf STARTED'],
                                cwd=self.root, capture_output=True, text=True, check=True)
        self.assertIn('symlink refused', result.stdout)
        self.assertIn('STARTED', result.stdout)
        self.assertTrue(plugin.is_symlink())
        self.assertEqual(custom.read_text(), 'custom')

if __name__ == '__main__':
    unittest.main()
