#!/usr/bin/env python3
"""Controlled Firefox six-tab/restart workload, preserving browser security.
Mac fixture only: public HTTPS, native RFB input/pixels and localhost JS evidence.
Acquire the project's heavy-test lease in the invoking worker.
"""
import argparse,hashlib,http.server,json,os,pathlib,subprocess,threading,urllib.parse,uuid
p=argparse.ArgumentParser(description=__doc__)
for name in ('runtime','rootfs','exchange','output','probe','user'):p.add_argument('--'+name,required=True)
p.add_argument('--hold-seconds',type=int,default=90);a=p.parse_args()
assert 30<=a.hold_seconds<=300
repo=pathlib.Path(__file__).resolve().parents[3];out=pathlib.Path(a.output).resolve();out.mkdir(exist_ok=False)
exchange=pathlib.Path(a.exchange).resolve();exchange.mkdir(parents=True,exist_ok=True)
state={'loads':[],'typed':'','clicked':False,'downloads':0,'scroll':0,'round':1};lock=threading.Lock();statefile=out/'http-state.json';download_name='linuxrunner-'+uuid.uuid4().hex+'.bin';payload=bytes((x*17)%251 for x in range(65536))
def save():
 temporary=statefile.with_suffix('.tmp');temporary.write_text(json.dumps(state));temporary.replace(statefile)
class Server(http.server.BaseHTTPRequestHandler):
 def log_message(self,*args):pass
 def do_GET(self):
  path=urllib.parse.urlsplit(self.path).path
  if path=='/download':
   self.send_response(200);self.send_header('Content-Type','application/octet-stream');self.send_header('Content-Disposition',f'attachment; filename="{download_name}"');self.send_header('Content-Length',str(len(payload)));self.end_headers();self.wfile.write(payload)
   with lock:state['downloads']+=1;save()
   return
  if path!='/browser':self.send_error(404);return
  page=b'''<!doctype html><html><body style="background:rgb(20,40,60);color:white;font:24px sans-serif;min-height:4000px"><h1>LinuxRunner Browser Probe</h1><input id="field" autofocus aria-label="Type here"><button id="button">Test input</button><p>JavaScript result 42</p><canvas id="canvas" width="400" height="120"></canvas><script>
let query=new URL(location.href).searchParams,r=Number(query.get('round')||1),tab=Number(query.get('tab')||1);function report(v){v.round=r;fetch('/event',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify(v)});}report({load:tab,result:6*7});field.addEventListener('input',()=>report({typed:field.value}));button.onclick=()=>report({clicked:true});window.onscroll=()=>report({scroll:Math.round(scrollY)});field.focus();let ctx=canvas.getContext('2d'),x=0;function draw(){ctx.fillStyle='#14283c';ctx.fillRect(0,0,400,120);ctx.fillStyle='#3f8';ctx.fillRect((x++%350),20,50,60);requestAnimationFrame(draw);}draw();
</script></body></html>'''
  self.send_response(200);self.send_header('Content-Type','text/html');self.send_header('Content-Length',str(len(page)));self.end_headers();self.wfile.write(page)
 def do_POST(self):
  if self.path!='/event':self.send_error(404);return
  length=int(self.headers.get('Content-Length','0'))
  if not 0<length<2048:self.send_error(400);return
  try:received=json.loads(self.rfile.read(length))
  except (ValueError,UnicodeError):self.send_error(400);return
  with lock:
   r=received.get('round');tab=received.get('load')
   if r in (1,2) and r!=state['round']:state.update(round=r,typed='',clicked=False,scroll=0)
   if r in (1,2) and tab in range(1,7) and received.get('result')==42:
    key=f'{r}-{tab}'
    if key not in state['loads']:state['loads'].append(key)
   if isinstance(received.get('typed'),str):state['typed']=received['typed'][:128]
   if received.get('clicked') is True:state['clicked']=True
   if isinstance(received.get('scroll'),int):state['scroll']=max(state['scroll'],received['scroll'])
   save()
  self.send_response(204);self.end_headers()
server=http.server.ThreadingHTTPServer(('127.0.0.1',0),Server);thread=threading.Thread(target=server.serve_forever,daemon=True);thread.start();base=f'http://127.0.0.1:{server.server_port}/browser';save()
profile='lr-tabs-'+uuid.uuid4().hex
fixture=exchange/(profile+'.sh');fixture.write_text(f'''#!/bin/sh
set -eu
profile="$HOME/.mozilla/{profile}"
mkdir -p "$profile" "$HOME/Downloads"
cat > "$profile/user.js" <<'PREFS'
user_pref("browser.aboutwelcome.enabled", false);
user_pref("datareporting.policy.firstRunURL", "");
user_pref("browser.startup.homepage_override.mstone", "ignore");
PREFS
: > "$HOME/browser-rounds.log"
for round in 1 2; do
 code=0
 firefox-esr --no-remote --new-instance --profile "$profile" '{base}?round='"$round"'&tab=1' || code=$?
 echo "BROWSER_ROUND_DONE $round status=$code" >> "$HOME/browser-rounds.log"
 [ "$code" = 0 ] || exit "$code"
done
''')
env=dict(os.environ,LR_BROWSER_STATE=str(statefile),LR_BROWSER_URL=base,LR_FIREFOX_HOLD=str(a.hold_seconds),LR_BROWSER_ROUNDS=str(pathlib.Path(a.rootfs)/'data/home'/a.user/'browser-rounds.log'),LR_BROWSER_DOWNLOAD=str(pathlib.Path(a.rootfs)/'data/home'/a.user/'Downloads'/download_name),LR_HOST_RESOURCE_METRICS='1',LR_PROBE_OCR_CPU='1')
try:
 r=subprocess.run(['python3',str(repo/'tests/linuxrunner/gui/run_guest_session.py'),'--runtime',a.runtime,'--rootfs',a.rootfs,'--exchange',str(exchange),'--output',str(out/'session'),'--mode','custom','--script',str(fixture),'--guest-user',a.user,'--prepare-support','--geometry','960x540','--glx','software','--probe',a.probe,'--timeout',str(a.hold_seconds*2+240)],cwd=repo,env=env,timeout=a.hold_seconds*2+300)
 files=list((pathlib.Path(a.rootfs)/'data/home'/a.user/'Downloads').glob(download_name.replace('.bin','*')))
 checks=[dict(bytes=f.stat().st_size,sha256=hashlib.file_digest(f.open('rb'),'sha256').hexdigest()) for f in files if f.is_file()]
 report=dict(exit=r.returncode,scope='Controlled Mac six-tab Firefox workload; not the reported physical-device thermal/crash workload',http_state=state,downloads=checks,expected_sha256=hashlib.sha256(payload).hexdigest(),downloads_match=bool(checks) and all(c['sha256']==hashlib.sha256(payload).hexdigest() for c in checks))
 (out/'result.json').write_text(json.dumps(report,indent=2)+'\n');raise SystemExit(r.returncode)
finally:server.shutdown();server.server_close();thread.join(3)
