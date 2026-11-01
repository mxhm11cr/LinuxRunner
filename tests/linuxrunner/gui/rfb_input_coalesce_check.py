# Fake RFB server for rfb_input_coalesce_probe.m. Build: clang -fobjc-arc -Iapp/LinuxRunner tests/linuxrunner/gui/rfb_input_coalesce_probe.m app/LinuxRunner/RFBClient.m app/LinuxRunner/RFBProtocol.c -framework Foundation -o probe
# Run: python3 tests/linuxrunner/gui/rfb_input_coalesce_check.py ./probe <per-pointer-read-delay-seconds>
import socket,struct,sys,subprocess,threading,json
binary=sys.argv[1];slow=float(sys.argv[2])
ls=socket.socket();ls.bind(('127.0.0.1',0));ls.listen(1);port=ls.getsockname()[1]
p=subprocess.Popen([binary,str(port)],stdout=subprocess.PIPE,text=True)
c,_=ls.accept()
def rd(n):
    b=b''
    while len(b)<n:
        x=c.recv(n-len(b))
        if not x: raise EOFError
        b+=x
    return b
c.sendall(b'RFB 003.008\n');rd(12);c.sendall(b'\x01\x02');rd(1);c.sendall(b'\0'*16);rd(16);c.sendall(b'\0\0\0\0');rd(1)
c.sendall(struct.pack('>HH',64,64)+bytes([32,24,0,1,0,255,0,255,0,255,16,8,0,0,0])+b'\0'+struct.pack('>I',1)+b'x')
import time;ev=[]
try:
    while True:
        t=rd(1)[0]
        if t==0: rd(19)
        elif t==2: rd(1);n=struct.unpack('>H',rd(2))[0];rd(4*n)
        elif t==3: rd(9)
        elif t==4: d=rd(7);ev.append(('k',d[0],struct.unpack('>I',d[3:])[0]))
        elif t==5:
            d=rd(5);ev.append(('p',d[0],struct.unpack('>HH',d[1:])))
            if slow: time.sleep(slow)
        else: print('bad',t);break
except EOFError: pass
print(p.communicate()[0].strip())
# expected sequence
sent=[]
for r in range(5):
    sent+= [('p',0,(i,r)) for i in range(1000)]+[('p',1,(1000,5))]+[('p',1,(1000+i,6)) for i in range(500)]+[('p',0,(1500,6))]
    sent+= [('k',1,97),('k',0,97)]+[('p',0,(2000+i,7)) for i in range(300)]
    for k in range(3): sent+=[('p',8,(2300,7)),('p',0,(2300,7))]
sent.append(('p',0,(4000,9)))
# received must be an ordered subsequence; every non-motion-equivalent event kept
j=0;ok=True
for e in ev:
    while j<len(sent) and sent[j]!=e: j+=1
    if j==len(sent): ok=False;break
    j+=1
def transitions(s):
    out=[];prev=None
    for e in s:
        key=(e[0],e[1]) if e[0]=='p' else e
        if e[0]=='k' or key!=prev: out.append(e[:2] if e[0]=='p' else e)
        prev=key
    return out
print(json.dumps({'sent':len(sent),'received':len(ev),'ordered_subsequence':ok,'transitions_preserved':transitions(sent)==transitions(ev),
 'final_ok':bool(ev) and ev[-1]==sent[-1],'presses':sum(1 for e in ev if e[0]=='p' and e[1]==1 and e[2]==(1000,5)),'wheel_events':sum(1 for e in ev if e[0]=='p' and e[1]==8),'keys':[e for e in ev if e[0]=='k'][:4]}))
