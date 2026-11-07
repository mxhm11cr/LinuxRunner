"""Exercise menu entries and launcher policy without a browser profile."""
import json
import os
import pathlib
import subprocess
import tempfile
import unittest

BUNDLE = pathlib.Path(__file__).resolve().parents[3] / 'app/LinuxRunner/GUISupport.bundle'


class FirefoxPolicyTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix='firefox policy ')
        self.addCleanup(self.temp.cleanup)
        self.path = pathlib.Path(self.temp.name)
        self.packages = self.path / 'packages'
        self.packages.mkdir()
        self.applications = self.path / 'managed/applications'
        self.support = self.path / 'support'
        self.support.mkdir()
        for name in ['firefox-desktop.sh', 'firefox-launch.sh', 'launch.sh', 'gpu-env.sh']:
            text = (BUNDLE / name).read_text().replace('/usr/share/applications', str(self.packages))
            (self.support / name).write_text(text)
        self.firefox = self.path / 'firefox-esr'
        self.firefox.write_text('''#!/usr/bin/env python3
import json,os,sys
keys=['GDK_BACKEND','MOZ_ENABLE_WAYLAND','LIBGL_ALWAYS_SOFTWARE','GALLIUM_DRIVER','LR_MESA_METAL_MODE','WAYLAND_DISPLAY','MESA_LOADER_DRIVER_OVERRIDE','DISPLAY','XAUTHORITY']
print(json.dumps({'environment':{key:os.environ.get(key) for key in keys},'arguments':sys.argv[1:]}))
''')
        self.firefox.chmod(0o755)
        self.env = dict(os.environ, GDK_BACKEND='wayland,x11', MOZ_ENABLE_WAYLAND='1',
                        WAYLAND_DISPLAY='/tmp/compositor/wayland-0', MESA_LOADER_DRIVER_OVERRIDE='kms_swrast',
                        LR_MESA_METAL_MODE='replace', DISPLAY=':8', XAUTHORITY='/tmp/xwayland-authority')

    def generate(self):
        subprocess.run(['/bin/sh', str(self.support / 'firefox-desktop.sh'), str(self.applications)], check=True)

    def test_policy_sets_x11_and_keeps_compositor_authorization(self):
        arguments = ['--new-window', 'file:///tmp/a b.html', 'literal;$(text)']
        result = subprocess.run(['/bin/sh', str(self.support / 'firefox-launch.sh'), str(self.firefox), *arguments],
                                env=self.env, text=True, capture_output=True, check=True)
        value = json.loads(result.stdout)
        self.assertEqual(value['arguments'], arguments)
        self.assertEqual(value['environment'], dict(GDK_BACKEND='x11', MOZ_ENABLE_WAYLAND='0',
                         LIBGL_ALWAYS_SOFTWARE='1', GALLIUM_DRIVER='softpipe', LR_MESA_METAL_MODE='off',
                         WAYLAND_DISPLAY=None, MESA_LOADER_DRIVER_OVERRIDE=None, DISPLAY=':8',
                         XAUTHORITY='/tmp/xwayland-authority'))

    def test_native_launcher_uses_the_same_policy(self):
        result = subprocess.run(['/bin/sh', str(self.support / 'launch.sh'), str(self.path), str(self.firefox), 'file:///tmp/test.html'],
                                env=self.env, text=True, capture_output=True, check=True)
        value = json.loads(result.stdout.splitlines()[-1])
        self.assertEqual(value['environment']['GDK_BACKEND'], 'x11')
        self.assertEqual(value['environment']['GALLIUM_DRIVER'], 'softpipe')
        self.assertEqual(value['arguments'], ['file:///tmp/test.html'])

    def test_menu_actions_preserve_arguments_and_disable_dbus_bypass(self):
        source = '[Desktop Entry]\nType=Application\nName=Firefox\nName[fr]=Firefox FR\nIcon=firefox-esr\nDBusActivatable=true\nExec=firefox-esr %u\nActions=private;\n[Desktop Action private]\nName=Private\nExec=firefox-esr --private-window %U\n'
        package = self.packages / 'firefox-esr.desktop'
        package.write_text(source)
        self.generate()
        value = (self.applications / package.name).read_text()
        self.assertEqual(package.read_text(), source)
        self.assertIn('DBusActivatable=false\n', value)
        self.assertIn('TryExec=firefox-esr\n', value)
        self.assertNotIn('DBusActivatable=true', value)
        self.assertIn('Name[fr]=Firefox FR\nIcon=firefox-esr', value)
        self.assertIn(f'Exec=/bin/sh "{self.support}/firefox-launch.sh" firefox-esr %u', value)
        self.assertIn('firefox-esr --private-window %U', value)

    def test_refresh_updates_managed_copy_and_removes_uninstalled_entry(self):
        package = self.packages / 'firefox.desktop'
        package.write_text('[Desktop Entry]\nType=Application\nExec=firefox %u\n')
        self.generate()
        self.generate()
        value = (self.applications / package.name).read_text()
        self.assertEqual(value.count('firefox-launch.sh'), 1)
        package.unlink()
        self.generate()
        self.assertFalse((self.applications / package.name).exists())

    def test_user_override_is_not_changed(self):
        user = self.path / 'user/applications/firefox.desktop'
        user.parent.mkdir(parents=True)
        user.write_text('custom user entry')
        (self.packages / 'firefox.desktop').write_text('[Desktop Entry]\nExec=firefox %u\n')
        self.generate()
        self.assertEqual(user.read_text(), 'custom user entry')

    def test_symbolic_link_destination_is_refused(self):
        self.applications.mkdir(parents=True)
        original = self.path / 'original'
        original.write_text('keep')
        (self.applications / 'firefox.desktop').symlink_to(original)
        result = subprocess.run(['/bin/sh', str(self.support / 'firefox-desktop.sh'), str(self.applications)], capture_output=True)
        self.assertNotEqual(result.returncode, 0)
        self.assertEqual(original.read_text(), 'keep')


if __name__ == '__main__':
    unittest.main()
