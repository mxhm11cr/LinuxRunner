#!/usr/bin/env python3
# OpenArena (ioquake3) frame timing on Ubuntu Xorg/glamor + virgl/ANGLE/Metal or Xvfb+llvmpipe (Mac host interpreter, not device).
import fcntl,subprocess,os,sys,re,statistics
import pathlib
# Paths: LR_WORK (workspace with fixtures/, gpu-deps/ and the GPU host runtime) and LR_RUNTIME.
L=os.environ.get('LR_WORK',os.path.expanduser('~/LinuxRunner-work'));F=L+'/fixtures/sprint-20261113'
mode,label,seconds=sys.argv[1],sys.argv[2],int(sys.argv[3])
env=dict(os.environ,ISH_VIRGL='1',ISH_SOFTWARE_DRM='1',ISH_BIND_MOUNTS='/x='+F+'/game-run')
server='/usr/lib/xorg/Xorg :9 -config /x/xorg.conf -logfile /x/xorg-%s.log -noreset -nolisten tcp -novtswitch -sharevts' % label if mode=='gpu' else 'Xvfb :9 -screen 0 960x540x24 -nolisten tcp +extension GLX'
gl='' if mode=='gpu' else 'LIBGL_ALWAYS_SOFTWARE=1 '
script=f'''rm -rf /root/.openarena; {server} > /x/server-{label}.log 2>&1 & xp=$!
i=0; while [ ! -S /tmp/.X11-unix/X9 ] && [ $i -lt 600 ]; do sleep .2; i=$((i+1)); done
export DISPLAY=:9
DISPLAY=:9 glxinfo -B 2>/dev/null | grep -E "renderer string|version string" | head -2
start=$(date +%s)
{gl}vblank_mode=0 timeout {seconds} /usr/games/openarena +set r_fullscreen 0 +set r_mode -1 +set r_customwidth 640 +set r_customheight 360 +set com_maxfps 0 +set r_swapInterval 0 +set s_initsound 0 +set com_speeds 1 +set logfile 2 +set com_introplayed 1 +devmap oa_dm1 > /x/game-{label}.out 2>&1
echo "game_exit=$? wall=$(( $(date +%s) - start ))"
cp /root/.openarena/baseoa/qconsole.log /x/qconsole-{label}.log 2>/dev/null; kill $xp; wait $xp 2>/dev/null'''
with open(L+'/archarm-recovery-test.lock','a') as l:
  fcntl.flock(l,fcntl.LOCK_EX)
  r=subprocess.run([os.environ.get('LR_RUNTIME',L+'/builds/virgl-pipeline-host-20261114/ish-direct'),'-f',F+'/ubuntu-base-24045','/bin/sh','-c',script],env=env,capture_output=True,text=True,timeout=seconds+900,cwd=L+'/gpu-deps/angle/out/mac-metal')
print(label,mode,'rc',r.returncode,[x for x in r.stdout.splitlines() if 'string' in x or 'game_exit' in x])
log=open(F+'/game-run/qconsole-'+label+'.log',errors='replace').read() if os.path.exists(F+'/game-run/qconsole-'+label+'.log') else ''
alls=[int(m) for m in re.findall(r'frame:\s*\d+\s+all:\s*(\d+)',log)]
print('frames logged',len(alls))
if alls:
  steady=alls[len(alls)//5:]   # skip map load and first frames
  ms=statistics.mean(steady); s=sorted(steady)
  print(f'steady frames {len(steady)} avg {ms:.1f} ms -> {1000/ms if ms else 0:.1f} FPS; p50 {s[len(s)//2]} ms p95 {s[int(len(s)*.95)]} ms')
print('\n'.join(l for l in log.splitlines() if re.search(r'GL_RENDERER|GL_VERSION|Loading|loaded|ERROR|Error',l))[:1500])
