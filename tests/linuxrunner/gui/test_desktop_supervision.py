#!/usr/bin/env python3
"""Desktop component supervision (desktop-components.sh) with stub components:
a crashing panel is restarted a bounded number of times, keeps its first failure
in panel.log, and then the session continues without it (degraded=panel); a
window manager that keeps crashing ends the session. Runs on the host with
/bin/sh and, when present, dash."""
import os, pathlib, shutil, subprocess, tempfile, time, unittest
BUNDLE = pathlib.Path(__file__).resolve().parents[3] / 'app/LinuxRunner/GUISupport.bundle'
LIVE = '#!/bin/sh\nexec sleep 300\n'
ABORT = '#!/bin/sh\necho "first failure $$" >&2\nkill -ABRT $$\n'
PROBE = '#!/bin/sh\nprintf "wm=1\\npanel=1\\nsurface=1\\nready=1\\n"\n'

class Supervision(unittest.TestCase):
    def run_session(self, shell, panel=LIVE, wm=LIVE, until='component_failed', seconds=40):
        root = pathlib.Path(tempfile.mkdtemp()); self.addCleanup(shutil.rmtree, root, True)
        gui = root / 'gui'; gui.mkdir(); stubs = root / 'bin'; stubs.mkdir(); home = root / 'home'; home.mkdir()
        for f in BUNDLE.iterdir():
            if f.is_file(): shutil.copy(f, gui / f.name)
        for name, body in {'openbox': wm, 'lxpanel': panel, 'pcmanfm': LIVE}.items():
            (stubs / name).write_text(body); (stubs / name).chmod(0o755)
        (gui / 'desktop-probe').write_text(PROBE); (gui / 'desktop-probe').chmod(0o755)
        session = gui / 'session-test'; session.mkdir()
        env = dict(os.environ, PATH=f'{stubs}:/usr/bin:/bin', HOME=str(home), XDG_DATA_HOME=str(home / 'data'),
                   XDG_CONFIG_HOME=str(home / 'config'), LR_DESKTOP_ENVIRONMENT='linuxrunner')
        log = root / 'application.log'
        with log.open('w') as out:
            p = subprocess.Popen([shell, str(gui / 'desktop-components.sh'), str(session)], env=env, stdout=out, stderr=subprocess.STDOUT)
            end = time.time() + seconds
            while time.time() < end and p.poll() is None and until not in log.read_text(): time.sleep(.2)
            time.sleep(1); (session / 'stop').touch()
            try: code = p.wait(timeout=15)
            except subprocess.TimeoutExpired: p.kill(); code = None
        subprocess.run(['pkill', '-f', str(stubs)], check=False)
        return code, log.read_text(), session

    def shells(self):
        return [s for s in ('/bin/sh', shutil.which('dash')) if s]

    def test_panel_crash_is_restarted_then_degraded(self):
        for shell in self.shells():
            with self.subTest(shell=shell):
                code, out, session = self.run_session(shell, panel=ABORT)
                self.assertEqual(out.count('component_restarted name=panel'), 3, out)
                self.assertIn('status=134', out)                      # SIGABRT reported, not hidden
                self.assertIn('component_failed name=panel (panel.log) restarts=3; the desktop continues without it', out)
                self.assertNotIn('ending the desktop session', out)
                self.assertEqual(code, 0, out)                         # ended by the stop request, not by the crash
                state = (session / 'desktop-state').read_text()
                self.assertIn('ready=0', state); self.assertIn('degraded=panel', state)
                panel_log = (session / 'panel.log').read_text()
                self.assertTrue(panel_log.startswith('first failure'), panel_log)   # first failure kept
                self.assertEqual(panel_log.count('=== LinuxRunner: restart'), 3)

    def test_window_manager_failure_ends_session(self):
        for shell in self.shells():
            with self.subTest(shell=shell):
                code, out, _ = self.run_session(shell, wm=ABORT, until='ending the desktop session')
                self.assertEqual(out.count('component_restarted name=window manager'), 3, out)
                self.assertIn('component_failed name=window manager (wm.log) restarts=3; ending the desktop session', out)
                self.assertEqual(code, 1, out)

    def test_i3_defaults_and_user_configuration(self):
        root = pathlib.Path(tempfile.mkdtemp()); self.addCleanup(shutil.rmtree, root, True)
        gui, stubs, home = (root / name for name in ('gui', 'bin', 'home'))
        for path in (gui, stubs, home): path.mkdir()
        for f in BUNDLE.iterdir():
            if f.is_file(): shutil.copy(f, gui / f.name)
        for path, body in ((stubs / 'i3', LIVE), (gui / 'desktop-probe', PROBE)):
            path.write_text(body); path.chmod(0o755)
        config = home / '.config/i3/config'
        env = dict(os.environ, PATH=f'{stubs}:/usr/bin:/bin', HOME=str(home),
                   XDG_CONFIG_HOME=str(home / '.config'), LR_DESKTOP_ENVIRONMENT='i3')
        for run in range(2):
            session = gui / f'session-{run}'; session.mkdir()
            log = root / f'i3-{run}.log'
            with log.open('w') as out:
                p = subprocess.Popen(['/bin/sh', str(gui / 'desktop-components.sh'), str(session)], env=env, stdout=out, stderr=subprocess.STDOUT)
                deadline = time.time() + 15
                while time.time() < deadline and p.poll() is None and 'environment=i3' not in log.read_text(): time.sleep(.1)
                self.assertIn('environment=i3', log.read_text())
                (session / 'stop').touch()
                try: p.wait(timeout=15)
                except subprocess.TimeoutExpired: p.kill(); p.wait(); self.fail('i3 session did not stop')
            if run == 0:
                self.assertIn('bindsym $mod+Return exec xterm', config.read_text())
                config.write_text('# user customization\n')
            else:
                self.assertEqual(config.read_text(), '# user customization\n')
        subprocess.run(['pkill', '-f', str(stubs)], check=False)

if __name__ == '__main__':
    unittest.main()
