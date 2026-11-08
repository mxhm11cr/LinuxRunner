#!/usr/bin/env python3
"""Exercise the production transport launcher without starting a display.

The guest integration reproduction separately uses real Xvfb, Hyprland and RFB.
Here a capture executable asserts environment/argv/PID and exit propagation,
including the important distinction between empty and absent WAYLAND_DISPLAY.
"""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

SCRIPT = Path(os.environ.get("LR_SESSION_SCRIPT", Path(__file__).resolve().parents[3] /
                            "app/LinuxRunner/GUISupport.bundle/session.sh"))


class TransportEnvironment(unittest.TestCase):
    def run_launcher(self, value, code=0, restarts=1):
        source = SCRIPT.read_text()
        function = "start_transport() {" + source.split("start_transport() {", 1)[1].split("\n}", 1)[0] + "\n}\n"
        with tempfile.TemporaryDirectory() as directory:
            work = Path(directory).resolve()
            executable = work / "x11vnc"
            executable.write_text('''#!/bin/sh
set -eu
[ "${WAYLAND_DISPLAY+x}" != x ] || exit 21
[ "$XDG_SESSION_TYPE" = x11 ] || exit 22
printf '%s\\n' "pid=$$" "$@" >> "$LR_CAPTURE"
exit "$LR_CAPTURE_EXIT"
''')
            executable.chmod(0o755)
            env = dict(os.environ, PATH=str(work)+":"+os.environ["PATH"],
                       XDG_SESSION_TYPE="wayland", LR_CAPTURE=str(work / "capture"),
                       LR_CAPTURE_EXIT=str(code))
            env.pop("WAYLAND_DISPLAY", None)
            if value is not None:
                env["WAYLAND_DISPLAY"] = value
            body = '''set -eu
session=$PWD; DISPLAY=:4711; XAUTHORITY=$PWD/authority; port=58123
vnc_lifetime=-forever; vnc_shm_option=-noshm; vnc_timing_options="-defer 16 -wait 25"
vnc_cursor_option=-nocursor; input_option=; xkb=-xkb
''' + function + f'''
i=0
while [ "$i" -lt {restarts} ]; do
 start_transport
 printf '%s\\n' "$vpid" >> pids
 result=0; wait "$vpid" || result=$?
 printf '%s\\n' "$result" >> exits
 i=$((i+1))
done
printf '%s\\n' "${{WAYLAND_DISPLAY+x}}" "${{WAYLAND_DISPLAY-unset}}" "$XDG_SESSION_TYPE" > parent
'''
            for shell in dict.fromkeys(filter(None, ("/bin/sh", shutil.which("dash")))):
                for name in ("capture", "pids", "exits", "parent"):
                    (work / name).unlink(missing_ok=True)
                subprocess.run([shell, "-ec", body], cwd=work, env=env, check=True, timeout=10)
                self.assertEqual((work / "exits").read_text().splitlines(), [str(code)] * restarts)
                self.assertEqual((work / "parent").read_text().splitlines(),
                                 ["x" if value is not None else "", value if value is not None else "unset", "wayland"])
                captured = (work / "capture").read_text().splitlines()
                pids = (work / "pids").read_text().splitlines()
                self.assertEqual([line[4:] for line in captured if line.startswith("pid=")], pids)
                self.assertEqual(captured.count(":4711"), restarts)
                self.assertEqual(captured.count(str(work / "authority")), restarts)
                self.assertEqual(captured.count("-localhost"), restarts)
                self.assertEqual(captured.count("-passwdfile"), restarts)

    def test_inherited_wayland_is_scoped_to_transport(self):
        self.run_launcher("wayland-existing")

    def test_empty_wayland_variable_is_removed_not_exported_empty(self):
        self.run_launcher("")

    def test_no_wayland_variable(self):
        self.run_launcher(None)

    def test_restart_and_real_failure_status(self):
        self.run_launcher("wayland-existing", code=29, restarts=2)


if __name__ == "__main__":
    unittest.main()
