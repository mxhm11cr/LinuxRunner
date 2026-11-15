#!/usr/bin/env python3
# glmark2 on a LinuxRunner desktop session (Mac host interpreter, not device). mode: gpu | software
import fcntl,subprocess,os,sys,re
import pathlib
# Paths: LR_WORK (workspace with fixtures/, gpu-deps/ and the GPU host runtime) and LR_RUNTIME.
L=os.environ.get('LR_WORK',os.path.expanduser('~/LinuxRunner-work'));F=L+'/fixtures/sprint-20261113'
mode,label=sys.argv[1],sys.argv[2]
env=dict(os.environ,ISH_VIRGL='1',ISH_SOFTWARE_DRM='1')
if mode=='gpu': env['LR_REQUIRE_NATIVE_XORG']='1'
out=F+'/glmark-'+label
scenes='-b build:use-vbo=true:duration=5 -b texture:texture-filter=linear:duration=5 -b shading:shading=phong:duration=5 -b terrain:duration=5 -b jellyfish:duration=5 -b refract:duration=5'
term=f'vblank_mode=0 LR_GUI_GPU_MODE={"mesa-metal-resident" if mode=="gpu" else "mesa"} timeout 150 /bin/sh /home/mobile/Downloads/.linuxrunner/gui/launch.sh "$HOME" /usr/bin/glmark2 --size 640x360 {scenes}'
cmd=['python3',str(pathlib.Path(__file__).resolve().parents[1]/'gui/run_guest_session.py'),'--rootfs',F+'/ubuntu-base-24045','--probe',L+'/builds/native-xorg-desktop-probe','--mode','desktop','--output',out,'--exchange',F+'/native-xorg-exchange','--runtime',os.environ.get('LR_RUNTIME',L+'/builds/virgl-pipeline-host-20261114/ish-direct'),'--guest-user','lrtest','--desktop-environment','linuxrunner','--geometry','960x540','--terminal-command',term,'--timeout','900']
if mode=='gpu': cmd[cmd.index('--output'):cmd.index('--output')]=['--gpu-mode','mesa-metal-resident','--native-xorg','--direct-scanout']
else: cmd[cmd.index('--output'):cmd.index('--output')]=['--gpu-mode','mesa','--glx','software']
with open(L+'/archarm-recovery-test.lock','a') as l:
  fcntl.flock(l,fcntl.LOCK_EX)
  r=subprocess.run(cmd,capture_output=True,text=True,timeout=1500,env=env,cwd=L+'/gpu-deps/angle/out/mac-metal')
t=open(out+'/terminal.log',errors='replace').read()
print(label,mode,'exit',r.returncode,'backend',open(out+'/display-backend').read().strip())
for line in t.splitlines():
  if re.search(r'GL_RENDERER|\] .*FPS:|glmark2 Score',line): print('  ',line.strip()[:150])
