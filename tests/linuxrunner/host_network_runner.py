#!/usr/bin/env python3
"""Controlled loopback echo servers, guest Linux sockets; no LAN probes.
Usage: python3 host_network_runner.py --runtime ... --rootfs ... --exchange ... --output ...
The rootfs must be disposable: gcc builds the copied contract in its exchange.
"""
import argparse,fcntl,json,os,pathlib,socket,subprocess,threading,time
p=argparse.ArgumentParser(description=__doc__)
for name in ('runtime','rootfs','exchange','output'):p.add_argument('--'+name,required=True)
a=p.parse_args();out=pathlib.Path(a.output)
if out.exists() and any(out.iterdir()):p.error('Use a new, empty output directory')
out.mkdir(parents=True,exist_ok=True)
exchange=pathlib.Path(a.exchange).resolve();exchange.mkdir(parents=True,exist_ok=True)
repo=pathlib.Path(__file__).resolve().parents[2]
(exchange/'host-network-contract.c').write_bytes((repo/'tests/regress/host-network-contract.c').read_bytes())
servers=[];stop=threading.Event();threads=[];ports=[]
def tcp(s):
 while not stop.is_set():
  try:client,_=s.accept()
  except (TimeoutError,OSError):continue
  with client:
   client.settimeout(2)
   try:
    while not stop.is_set():
     b=client.recv(65536)
     if not b:break
     client.sendall(b)
   except (TimeoutError,OSError):pass
def udp(s):
 while not stop.is_set():
  try:b,addr=s.recvfrom(65536);s.sendto(b,addr)
  except (TimeoutError,OSError):continue
for family in (socket.AF_INET,socket.AF_INET6):
 for kind in (socket.SOCK_STREAM,socket.SOCK_DGRAM):
  s=socket.socket(family,kind)
  try:s.bind(('127.0.0.1' if family==socket.AF_INET else '::1',0))
  except OSError:
   s.close();ports.append(0);continue
  s.settimeout(.2)
  if kind==socket.SOCK_STREAM:s.listen(32)
  servers.append(s);ports.append(s.getsockname()[1]);t=threading.Thread(target=tcp if kind==socket.SOCK_STREAM else udp,args=(s,),daemon=True);t.start();threads.append(t)
# A closed ephemeral port is verified natively as refusing connections.
# Darwin can silently drop SYNs to a bound-but-not-listening socket.
refused=socket.socket();refused.bind(('127.0.0.1',0));ports.append(refused.getsockname()[1]);refused.close()
probe=socket.socket();probe.settimeout(1)
assert probe.connect_ex(('127.0.0.1',ports[-1])) == __import__('errno').ECONNREFUSED
probe.close()
env=dict(os.environ,ISH_NETLINK_STUB='1',ISH_BIND_MOUNTS='/home/mobile/Downloads='+str(exchange))
command='set -eu; cd /home/mobile/Downloads; gcc -O2 -Wall host-network-contract.c -o host-network-contract; ./host-network-contract '+' '.join(map(str,ports))
started=time.monotonic()
try:
 with (pathlib.Path(a.rootfs)/'.host-gui-test.lock').open('a') as fixture_lock:
  fcntl.flock(fixture_lock,fcntl.LOCK_EX|fcntl.LOCK_NB)
  with (out/'guest.log').open('w') as log:
   r=subprocess.run([str(pathlib.Path(a.runtime).resolve()),'-f',str(pathlib.Path(a.rootfs).resolve()),'/bin/sh','-ec',command],env=env,stdout=log,stderr=subprocess.STDOUT,timeout=90)
 data=(out/'guest.log').read_text(errors='replace');status='PASS' if r.returncode==0 and 'NETWORK_CONTRACT_PASS' in data else 'FAIL'
 record=dict(status=status,exit=r.returncode,environment='macOS host interpreter + controlled host loopback TCP/UDP servers; not iOS/LAN',ipv6=bool(ports[2]),elapsed_seconds=time.monotonic()-started,runtime=str(pathlib.Path(a.runtime).resolve()))
 (out/'result.json').write_text(json.dumps(record,indent=2)+'\n');print(json.dumps(record));raise SystemExit(0 if status=='PASS' else 1)
finally:
 stop.set();refused.close()
 for s in servers:s.close()
 for t in threads:t.join(3)
