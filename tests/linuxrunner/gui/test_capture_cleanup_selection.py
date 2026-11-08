"""Exercise session capture selection with disposable capability stubs."""
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

BUNDLE = Path(__file__).resolve().parents[3] / "app/LinuxRunner/GUISupport.bundle"
SESSION = (BUNDLE / "session.sh").read_text()
SELECTION = SESSION[SESSION.index("shm_mode=off\n"):SESSION.index("# Wayfire draws")]


class CaptureSelection(unittest.TestCase):
    def select(self, requested=None, helper=None):
        with tempfile.TemporaryDirectory() as temporary:
            base = Path(temporary)
            session = base / "session"
            session.mkdir()
            if requested is not None:
                (session / "shm-mode").write_text(requested)
            if helper is not None:
                guard = base / "capture-shm-guard.arm64"
                guard.write_text(f'#!/bin/sh\n[ "$1" = --probe ] || exit 99\nexit {helper}\n')
                guard.chmod(0o755)
            return subprocess.run(["/bin/sh", "-ec", SELECTION +
                '\nprintf "RESULT mode=%s x11=%s vnc=%s launcher=%s\\n" '
                '"$shm_mode" "$shm_switch" "$vnc_shm_option" "$capture_launcher"\n'],
                cwd=base, env={**os.environ, "session": str(session)},
                capture_output=True, text=True)

    def test_default_keeps_socket(self):
        result = self.select(helper=0)
        self.assertEqual(result.returncode, 0)
        self.assertIn("RESULT mode=off x11=-extension vnc=-noshm launcher=\n", result.stdout)

    def test_explicit_shared_uses_capable_launcher(self):
        result = self.select("sysv", 0)
        self.assertEqual(result.returncode, 0)
        self.assertIn("RESULT mode=sysv x11=+extension vnc= launcher=./capture-shm-guard.arm64", result.stdout)

    def test_missing_or_rejected_capability_falls_back(self):
        for helper in (None, 77, 1):
            with self.subTest(helper=helper):
                result = self.select("sysv", helper)
                self.assertEqual(result.returncode, 0)
                self.assertIn("cleanup unavailable; using socket capture", result.stdout)
                self.assertIn("RESULT mode=off x11=-extension vnc=-noshm launcher=\n", result.stdout)

    def test_explicit_socket_is_preserved(self):
        result = self.select("off", 99)
        self.assertEqual(result.returncode, 0)
        self.assertNotIn("cleanup unavailable", result.stdout)
        self.assertIn("RESULT mode=off", result.stdout)

    def test_bad_mode_still_rejected(self):
        self.assertEqual(self.select("unknown", 0).returncode, 2)


if __name__ == "__main__":
    unittest.main()
