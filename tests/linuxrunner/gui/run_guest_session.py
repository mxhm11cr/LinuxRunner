#!/usr/bin/env python3
"""Run production GUI helpers in a disposable fakefs host fixture (not device proof)."""
import argparse,pathlib,subprocess,secrets,socket,uuid,os,json,fcntl,hashlib,shutil
from fixture_identity import check_identity, load_profile, check_load_bases
p=argparse.ArgumentParser();p.add_argument('--rootfs',required=True);p.add_argument('--probe',required=True);p.add_argument('--mode',choices=['test','xterm','qt','custom','desktop'],required=True);p.add_argument('--gpu-mode',choices=['mesa','mesa-metal-resident','mesa-metal-verify'],help='Settings › Graphics session default (session file gpu-mode)');p.add_argument('--output',required=True);p.add_argument('--xkb',choices=['xkb','noxkb'],default='xkb');p.add_argument('--script');p.add_argument('--baseline-display',action='store_true');p.add_argument('--baseline-input',action='store_true',help='Legacy input handling (default; comparison alias)');p.add_argument('--eager-input',action='store_true',help='Opt in to experimental x11vnc eager input handling');p.add_argument('--exchange');p.add_argument('--glx',choices=['off','software'],default=None);p.add_argument('--geometry',type=lambda g:g if __import__('re').fullmatch(r'[1-9][0-9]{2,3}x[1-9][0-9]{2,3}',g) else p.error('geometry WxH'),default='640x480',help='WxH, as the app passes (fixed sizes or Fit)');p.add_argument('--timeout',type=int,default=180);p.add_argument('--runtime',help='Test-only alternate host driver for bootstrap comparisons');p.add_argument('--prepare-support',action='store_true',help='Compile verified GUI fixture helpers without installing packages');p.add_argument('--shared-images',choices=['off','sysv'],default='off',help='Experimental guest X11 shared-image capture; default preserves socket-image rollback');p.add_argument('--guest-user',default='root',help='Test the selected guest identity through the production su/session path');p.add_argument('--application-bundles',help='Test-only read-only host directory mounted at /opt/linuxrunner-fixture-apps-874');p.add_argument('--vnc-timing',choices=['default','legacy','threads','fast','damage','fast-damage','adaptive','adaptive-slow','balanced','threads-balanced'],default='default',help='Experimental x11vnc update policy for latency comparisons');p.add_argument('--audio-port',type=int,help='Session sound: loopback port for the PulseAudio monitor stream (the app chooses one)');p.add_argument('--terminal-command',help='Desktop mode: once the desktop is ready, run this through su -l <guest user> from a separate process tree (as the iOS terminal does); output in terminal.log');p.add_argument('--terminal-root-command',help='Same, as root through a login shell');p.add_argument('--desktop-environment',choices=['linuxrunner','plasma','lxqt','enlightenment','wayfire','hyprland'],help='Desktop mode: Settings › Desktop Environment (session file desktop-environment)');p.add_argument('--black-screen-diagnostics',action='store_true',help='Explicit bounded root process/wait snapshots; slows diagnostic runs, not benchmarks');a=p.parse_args()
if shutil.disk_usage(a.rootfs).free < 256*1024*1024:
 raise SystemExit('GUI_FIXTURE_STORAGE_LOW: preserve/relocate completed build artifacts before testing (minimum 256 MiB free)')
root=pathlib.Path(__file__).resolve().parents[3];out=pathlib.Path(a.output).resolve()
if out.exists() and any(out.iterdir()):raise SystemExit('STALE_TRACE_REJECTED: use a new, empty output directory')
out.mkdir(parents=True,exist_ok=True)
profile_path=os.environ.get('LR_FIXTURE_IDENTITY_PROFILE')
profile=load_profile(profile_path) if profile_path else None
identity=check_identity(a.rootfs,profile) if profile else None
# Bind mounts in this host fixture change its backing symlink. Concurrent
# sessions with different exchange paths would mutate a running guest's view.
fixture_lock=(pathlib.Path(a.rootfs)/'.host-gui-test.lock').open('a')
try:fcntl.flock(fixture_lock,fcntl.LOCK_EX|fcntl.LOCK_NB)
except BlockingIOError:raise SystemExit('GUI fixture already in use; clone it or wait for its owned session')
exchange=pathlib.Path(a.exchange).resolve() if a.exchange else root/'build/gui-exchange'
support=exchange/'.linuxrunner/gui';support.mkdir(parents=True,exist_ok=True)
# Mirror packaged resource deployment, not just the two session helpers. In
# particular the checksum manifest must describe the session being exercised.
for source in (root/'app/LinuxRunner/GUISupport.bundle').iterdir():
 if source.is_file():
  destination=support/source.name;destination.write_bytes(source.read_bytes())
  destination.chmod(0o755 if source.suffix=='.sh' else 0o644)
if a.baseline_display:
 script=(support/'session.sh').read_text().replace('-defer 16 -wait 25','-defer 80 -wait 80');(support/'session.sh').write_text(script)
session=support/('session-'+str(uuid.uuid4()));session.mkdir(mode=0o700)
s=socket.socket();s.bind(('127.0.0.1',0));port=s.getsockname()[1];s.close()
for n,t in {'guest-user':a.guest_user,'port':str(port),'display':str(secrets.randbelow(4000)+1000),'cookie':secrets.token_hex(16),'password':''.join(secrets.choice('ABCDEFGHJKLMNPQRSTUVWXYZabcdefghijkmnopqrstuvwxyz23456789') for _ in range(8)),'xkb-mode':a.xkb,'geometry':a.geometry}.items():(session/n).write_text(t);(session/n).chmod(0o600)
glx=a.glx or os.environ.get('LR_FIXTURE_GLX','off')
if glx not in ('off','software'):raise SystemExit('invalid LR_FIXTURE_GLX')
(session/'glx-mode').write_text(glx);(session/'glx-mode').chmod(0o600)
if a.gpu_mode:(session/'gpu-mode').write_text(a.gpu_mode);(session/'gpu-mode').chmod(0o600)
if a.desktop_environment:(session/'desktop-environment').write_text(a.desktop_environment);(session/'desktop-environment').chmod(0o600)
(session/'shm-mode').write_text(a.shared_images);(session/'shm-mode').chmod(0o600)
if a.script:(session/'command.sh').write_bytes(pathlib.Path(a.script).read_bytes());(session/'command.sh').chmod(0o600)
(root/'build/qt-tests').mkdir(parents=True, exist_ok=True)
(root/'build/qt-tests/current-session.json').write_text(json.dumps({'path':str(session),'port':port}))
env=dict(os.environ,ISH_NETLINK_STUB='1',ISH_BIND_MOUNTS='/home/mobile/Downloads='+str(exchange))
if a.application_bundles:
 bundles=pathlib.Path(a.application_bundles).resolve()
 if not bundles.is_dir() or ',' in str(bundles):raise SystemExit('Invalid application bundle directory')
 env['ISH_BIND_MOUNTS']+=',/opt/linuxrunner-fixture-apps-874='+str(bundles)+':ro'
runtime=pathlib.Path(a.runtime).resolve() if a.runtime else root/'build/host/ish'
probe=pathlib.Path(a.probe).resolve()
if a.prepare_support:
 # Fakefs execute modes come from its metadata database, not host chmod.
 # A fresh exchange needs helpers built through the guest before session.sh.
 preparation = """set -eu
 cd /home/mobile/Downloads/.linuxrunner/gui
 . ./support-common.sh
 verify_resources
 pkg-config --exists x11 xcb
 gcc gui-test.c -O2 -Wall -Wextra $(pkg-config --cflags --libs x11 xcb) -o gui-test.fixture
 gcc focus-manager.c -O2 -Wall -Wextra -lX11 -o focus-manager.fixture
 chmod 755 gui-test.fixture focus-manager.fixture
 verify_elf GUI_FIXTURE "$PWD/gui-test.fixture"
 verify_elf GUI_FIXTURE "$PWD/focus-manager.fixture"
 mv gui-test.fixture gui-test
 mv focus-manager.fixture focus-manager
 /bin/sh ./prepare-window-observer.sh
 test -x ./gui-test && test -x ./focus-manager && test -x ./window-observer
 echo GUI_FIXTURE_SUPPORT_PASS
 """
 if a.mode=='desktop':
  preparation += "\ngcc desktop-probe.c -O2 -Wall -Wextra -lX11 -o desktop-probe.fixture\nchmod 755 desktop-probe.fixture\nverify_elf GUI_FIXTURE \"$PWD/desktop-probe.fixture\"\nmv desktop-probe.fixture desktop-probe\n"
 with (out/'prepare-support.log').open('w') as log:
  subprocess.run([str(runtime),'-f',str(pathlib.Path(a.rootfs).resolve()),'/bin/sh','-ec',preparation,'/home/mobile/Downloads/.linuxrunner/gui/fixture-prepare'],env=env,stdout=log,stderr=subprocess.STDOUT,timeout=120,check=True)
if a.baseline_input and a.eager_input:raise SystemExit('Choose legacy or eager input, not both')
(session/'input-mode').write_text('eager' if a.eager_input else 'legacy');(session/'input-mode').chmod(0o600)
(session/'vnc-timing').write_text(a.vnc_timing);(session/'vnc-timing').chmod(0o600)
execution=dict(vnc_timing=a.vnc_timing,shared_images=a.shared_images,input_mode='eager' if a.eager_input else 'legacy',glx_mode=glx,runtime=str(runtime),runtime_sha256=hashlib.sha256(runtime.read_bytes()).hexdigest(),probe=str(probe),probe_sha256=hashlib.sha256(probe.read_bytes()).hexdigest(),source_revision=subprocess.check_output(['git','rev-parse','HEAD'],cwd=root,text=True).strip(),source_worktree_dirty=bool(subprocess.check_output(['git','status','--porcelain'],cwd=root,text=True).strip()))
(out/'execution-identity.json').write_text(json.dumps(execution,indent=2)+'\n')
print('GUI_FIXTURE_RUNTIME', runtime, 'sha256='+execution['runtime_sha256'], flush=True)
print('GUI_FIXTURE_PROBE', probe, 'sha256='+execution['probe_sha256'], flush=True)
if identity:
 identity.update(**execution, mode=a.mode, geometry=a.geometry, xkb=a.xkb, session_id=session.name, platform='macOS host interpreter')
 (out/'fixture-identity.json').write_text(json.dumps(identity,indent=2)+'\n')
launch=['/bin/sh','/home/mobile/Downloads/.linuxrunner/gui/session.sh',session.name,a.mode]
if a.black_screen_diagnostics:
 (exchange/'runtime_snapshot.c').write_bytes((root/'tests/linuxrunner/gui/runtime_snapshot.c').read_bytes())
 diagnostic_env=dict(env);diagnostic_env.pop('ISH_SYSTRACE',None)
 with (out/'diagnostic-build.log').open('w') as log:
  subprocess.run([str(runtime),'-f',str(pathlib.Path(a.rootfs).resolve()),'/bin/sh','-ec','cd /home/mobile/Downloads; gcc -O2 -Wall -Wextra runtime_snapshot.c -lX11 -o runtime-snapshot'],env=diagnostic_env,stdout=log,stderr=subprocess.STDOUT,timeout=60,check=True)
 (session/'black-screen-snapshot.sh').write_bytes((root/'tests/linuxrunner/gui/black_screen_snapshot.sh').read_bytes())
 (session/'diagnostic-launch.sh').write_text('''#!/bin/sh
set -eu
session=$1
/bin/sh "$session/black-screen-snapshot.sh" "$session" &
exec /bin/sh /home/mobile/Downloads/.linuxrunner/gui/session.sh "$2" "$3"
''')
 guest_session='/home/mobile/Downloads/.linuxrunner/gui/'+session.name
 launch=['/bin/sh',guest_session+'/diagnostic-launch.sh',guest_session,session.name,a.mode]
if a.audio_port:(session/'audio-port').write_text(str(a.audio_port))
if a.terminal_command or a.terminal_root_command:
 # Terminal/desktop integration: an independent login shell, started after
 # the desktop reports ready, must reach it through the profile hook only.
 guest_session='/home/mobile/Downloads/.linuxrunner/gui/'+session.name
 (session/'terminal-command').write_text(a.terminal_command or '')
 (session/'terminal-root-command').write_text(a.terminal_root_command or '')
 (session/'terminal-launch.sh').write_text('''#!/bin/sh
set -eu
session=$1 user=$2
(
 i=0; until grep -q ready=1 "$session/desktop-state" 2>/dev/null; do i=$((i+1)); [ "$i" -lt 900 ] || exit 0; sleep 1; done
 sleep 3
 { echo "TERMINAL_TEST user=$user"; if [ -s "$session/terminal-command" ]; then su -l "$user" -c "$(cat "$session/terminal-command")" || echo "TERMINAL_TEST user_exit=$?"; fi
   echo "TERMINAL_TEST root"; if [ -s "$session/terminal-root-command" ]; then env -i HOME=/root TERM=xterm-256color /bin/sh -l -c "$(cat "$session/terminal-root-command")" || echo "TERMINAL_TEST root_exit=$?"; fi
   echo TERMINAL_TEST_DONE; } > "$session/terminal.log" 2>&1
 touch "$session/terminal.done"
) &
exec /bin/sh /home/mobile/Downloads/.linuxrunner/gui/session.sh "$3" "$4"
''')
 launch=['/bin/sh',guest_session+'/terminal-launch.sh',guest_session,a.guest_user or 'root',session.name,a.mode]
with (out/'launch.log').open('w') as log:
 guest=subprocess.Popen([str(runtime),'-f',str(pathlib.Path(a.rootfs).resolve())]+launch,env=env,stdout=log,stderr=subprocess.STDOUT)
 metrics=None
 try:
  if os.environ.get('LR_HOST_RESOURCE_METRICS')=='1':
   from runtime_metrics import RuntimeMetrics
   metrics=RuntimeMetrics(guest.pid,out/'host-runtime-metrics.json');metrics.start()
  result=subprocess.run([str(pathlib.Path(a.probe).resolve()),str(port),str(session/'password'),str(out),a.mode],timeout=a.timeout,env=dict(env,LR_GUI_APPLICATION_LOG=str(session/'application.log'),LR_GUI_RUNTIME=str(runtime),LR_GUI_ROOTFS=str(pathlib.Path(a.rootfs).resolve()),LR_GUI_GUEST_USER=a.guest_user))
  (session/"stop").touch();guest.wait(timeout=10)
 finally:
  if metrics:metrics.stop()
  (session/'stop').touch()
  if guest.poll() is None:
   try:guest.wait(timeout=5)
   except subprocess.TimeoutExpired:guest.terminate();guest.wait(timeout=5)
  for n in ('session.log','xvfb.log','display.log','application.log','focus.log','window.log','window-state','keymap.log','bus.log','wm.log','panel.log','desktop.log','plasma.log','desktop-state','xfconf.log','runtime-snapshots.log','process-snapshots.log','fd-trends.log','terminal.log','desktop-state.probe'):
   if (session/n).exists():(out/n).write_bytes((session/n).read_bytes())
  if (session/'gpu-stats').is_dir():   # per-process bridge statistics (Metal vs Mesa draws)
   (out/'gpu-stats').mkdir(exist_ok=True)
   for f in (session/'gpu-stats').glob('*.txt'):(out/'gpu-stats'/f.name).write_bytes(f.read_bytes())
if a.black_screen_diagnostics:
 from black_screen_diagnostic import write_report
 write_report(out)
if profile:
 if check_identity(a.rootfs,profile)['files']!=identity['files']:raise SystemExit('STALE_TRACE_REJECTED: files changed during session')
 if profile.get('load_base') or profile.get('load_bases'):
  check_load_bases((out/'application.log').read_text(errors='replace'),profile)
print('probe',result.returncode,'supervisor',guest.returncode)
raise SystemExit(result.returncode or guest.returncode)
