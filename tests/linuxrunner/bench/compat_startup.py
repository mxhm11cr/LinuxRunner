#!/usr/bin/env python3
"""Interleaved baseline/candidate host-driver wall times, not iPhone timings.
Compile tests/linuxrunner/benchmark_compute.c to the supplied exchange as
compat-compute using the guest gcc first. The two runtimes use the same fixture.
"""
import argparse,fcntl,hashlib,json,os,pathlib,re,statistics,subprocess,time
p=argparse.ArgumentParser(description=__doc__)
for key in ('baseline','candidate','rootfs','exchange','output'):p.add_argument('--'+key,required=True)
p.add_argument('--samples',type=int,default=7)
a=p.parse_args();root=pathlib.Path(a.rootfs).resolve();exchange=pathlib.Path(a.exchange).resolve()
env=dict(os.environ,ISH_BIND_MOUNTS='/home/mobile/Downloads='+str(exchange),ISH_NETLINK_STUB='1')
workloads={'startup':['/bin/true'],'shell_10_exec':['/bin/sh','-ec','i=0; while [ "$i" -lt 10 ]; do /bin/true; i=$((i+1)); done'], 'compute':['/home/mobile/Downloads/compat-compute']}
results={};digests=set()
with (root/'.host-gui-test.lock').open('a') as lease:
 fcntl.flock(lease,fcntl.LOCK_EX|fcntl.LOCK_NB)
 for name,command in workloads.items():
  rows={'baseline':[],'candidate':[]}
  for sample in range(a.samples):
   for variant in (['baseline','candidate'] if sample%2==0 else ['candidate','baseline']):
    runtime=str(pathlib.Path(getattr(a,variant)).resolve());started=time.perf_counter()
    r=subprocess.run([runtime,'-f',str(root),*command],env=env,capture_output=True,text=True,timeout=20);elapsed=time.perf_counter()-started
    if r.returncode:raise RuntimeError((name,variant,r.returncode,r.stderr[-1000:]))
    rows[variant].append(elapsed)
    if name=='compute':
     match=re.search(r'compute=[0-9a-f]+:[0-9a-f]+',r.stdout)
     if not match:raise RuntimeError('Missing compute digest')
     digests.add(match.group())
  results[name]={variant:dict(seconds=values,median_seconds=statistics.median(values)) for variant,values in rows.items()}
 if len(digests)!=1:raise RuntimeError('Compute result changed')
report=dict(environment='macOS ARM64 LinuxRunner interpreter host-driver wall time; not iPhone',scope='whole command including runtime initialization; warm filesystem cache, interleaved repeats, no JIT',rootfs=str(root),samples=a.samples,compute_digest=next(iter(digests)),runtimes={v:dict(path=getattr(a,v),sha256=hashlib.sha256(pathlib.Path(getattr(a,v)).read_bytes()).hexdigest()) for v in ['baseline','candidate']},results=results)
pathlib.Path(a.output).write_text(json.dumps(report,indent=2)+'\n');print(json.dumps(report,indent=2))
