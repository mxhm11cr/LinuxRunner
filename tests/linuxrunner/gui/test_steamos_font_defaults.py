"""Preserve explicit account settings when choosing virtual-display font defaults."""
import json
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

SCRIPT = Path(__file__).resolve().parents[3] / "app/LinuxRunner/GUISupport.bundle/steamos-fonts.sh"


class SteamOSFontDefaults(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.base = Path(self.temp.name)
        self.config = self.base / "config"
        self.config.mkdir()
        self.file = self.config / "kdeglobals"
        self.calls = self.base / "calls.json"
        self.bin = self.base / "bin"
        self.bin.mkdir()
        tool = self.bin / "kwriteconfig6"
        tool.write_text("#!/usr/bin/env python3\nimport json,os,sys\nfrom pathlib import Path\nPath(os.environ['FONT_TEST_CALLS']).write_text(json.dumps(sys.argv[1:]))\n")
        tool.chmod(0o755)

    def run_defaults(self):
        return subprocess.run(["/bin/sh", str(SCRIPT)], check=True, capture_output=True, text=True,
                              env=dict(os.environ, PATH=f"{self.bin}:{os.environ['PATH']}",
                                       XDG_CONFIG_HOME=str(self.config), FONT_TEST_CALLS=str(self.calls)))

    def test_default_requests_only_grayscale_key(self):
        self.file.write_text("[General]\nColorScheme=Custom\n")
        self.run_defaults()
        self.assertEqual(json.loads(self.calls.read_text()),
                         ["--file", str(self.file), "--group", "General", "--key", "XftSubPixel", "none"])

    def test_missing_file_can_receive_default(self):
        self.run_defaults()
        self.assertTrue(self.calls.exists())

    def test_explicit_preferences_are_preserved(self):
        for key in ["XftSubPixel", "XftSubPixel[$i]", "XftSubPixel[$e]", "XftSubPixel[$i][$e]"]:
            for value in ["rgb", "none", "bgr"]:
                original = f"[General]\n{key}={value}\n"
                self.file.write_text(original)
                self.run_defaults()
                self.assertFalse(self.calls.exists())
                self.assertEqual(self.file.read_text(), original)

    def test_symlink_is_preserved(self):
        target = self.base / "custom"
        target.write_text("custom file")
        self.file.symlink_to(target)
        self.run_defaults()
        self.assertFalse(self.calls.exists())
        self.assertEqual(target.read_text(), "custom file")

    def test_existing_directory_is_preserved(self):
        self.file.mkdir()
        self.run_defaults()
        self.assertFalse(self.calls.exists())

    def test_unavailable_configuration_directory_is_nonfatal(self):
        self.config.rmdir()
        self.config.write_text("user file")
        self.run_defaults()
        self.assertFalse(self.calls.exists())
        self.assertEqual(self.config.read_text(), "user file")


if __name__ == "__main__":
    unittest.main()
