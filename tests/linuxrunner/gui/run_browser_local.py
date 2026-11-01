#!/usr/bin/env python3
"""Controlled real-browser smoke; no sandbox/security disabling flags.
Requires an existing standard guest account and compiled browser_local_probe.
Use disposable fixtures. Whole-fixture memory samples are optional via
LR_HOST_RESOURCE_METRICS=1; these are host-driver, never physical-device RSS.
"""
import argparse,fcntl,http.server,json,os,pathlib,subprocess,threading,urllib.parse,uuid
p=argparse.ArgumentParser(description=__doc__)
for key in ('runtime','rootfs','exchange','output','probe','user'):p.add_argument('--'+key,required=True)
p.add_argument('--browser',choices=['firefox','chromium'],required=True);a=p.parse_args()
repo=pathlib.Path(__file__).resolve().parents[3];out=pathlib.Path(a.output).resolve()
if out.exists() and any(out.iterdir()):p.error('Use a new, empty output directory')
out.mkdir(parents=True,exist_ok=True)
exchange=pathlib.Path(a.exchange).resolve();exchange.mkdir(parents=True,exist_ok=True)
state={'loads':[],'typed':'','clicked':False};state_lock=threading.Lock();state_file=out/'http-state.json'
def save():
 temporary=state_file.with_suffix('.tmp');temporary.write_text(json.dumps(state));temporary.replace(state_file)
class Server(http.server.BaseHTTPRequestHandler):
 def log_message(self,*args):pass
 def do_GET(self):
  if urllib.parse.urlsplit(self.path).path!='/browser':self.send_error(404);return
  page=b'''<!doctype html><html><body style="background:rgb(20,40,60);color:white;font:24px sans-serif"><h1>LinuxRunner Browser Probe</h1><input id="field" autofocus aria-label="Type here"><button id="button">Test input</button><p>JavaScript result 42</p><script>
let tab=Number(new URL(location.href).searchParams.get('tab')||1);
function report(value){fetch('/event',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify(value)});}
report({load:tab,result:6*7});field.addEventListener('input',()=>report({typed:field.value}));button.onclick=()=>report({clicked:true});field.focus();
</script></body></html>'''
  self.send_response(200);self.send_header('Content-Type','text/html');self.send_header('Content-Length',str(len(page)));self.end_headers();self.wfile.write(page)
 def do_POST(self):
  if self.path!='/event':self.send_error(404);return
  length=int(self.headers.get('Content-Length','0'))
  if not 0<length<2048:self.send_error(400);return
  try:received=json.loads(self.rfile.read(length))
  except (ValueError,UnicodeError):self.send_error(400);return
  with state_lock:
   if received.get('result')==42 and received.get('load') in [1,2,3] and received['load'] not in state['loads']:state['loads'].append(received['load'])
   if isinstance(received.get('typed'),str):state['typed']=received['typed'][:128]
   if received.get('clicked') is True:state['clicked']=True
   save()
  self.send_response(204);self.end_headers()
server=http.server.ThreadingHTTPServer(('127.0.0.1',0),Server);thread=threading.Thread(target=server.serve_forever,daemon=True);thread.start();url=f'http://127.0.0.1:{server.server_port}/browser';save()
profile='lr-compat-'+uuid.uuid4().hex
if a.browser=='firefox':
 script=f'''#!/bin/sh
set -eu
profile="$HOME/.mozilla/{profile}"
mkdir -p "$profile"
cat > "$profile/user.js" <<'PREFS'
user_pref("browser.aboutwelcome.enabled", false);
user_pref("datareporting.policy.firstRunURL", "");
user_pref("browser.startup.homepage_override.mstone", "ignore");
PREFS
exec /usr/bin/firefox-esr --no-remote --new-instance --profile "$profile" '{url}'
'''
else:script=f'''#!/bin/sh
set -eu
exec /usr/bin/chromium --no-first-run --user-data-dir="$HOME/.config/{profile}" '{url}'
'''
fixture=exchange/('browser-'+profile+'.sh');fixture.write_text(script)
env=dict(os.environ,LR_BROWSER_STATE=str(state_file),LR_BROWSER_URL=url)
try:
 args=['python3',str(repo/'tests/linuxrunner/gui/run_guest_session.py'),'--runtime',a.runtime,'--rootfs',a.rootfs,'--exchange',str(exchange),'--output',str(out/'session'),'--mode','custom','--script',str(fixture),'--guest-user',a.user,'--geometry','960x540','--glx','software','--probe',a.probe,'--timeout','80']
 r=subprocess.run(args,cwd=repo,env=env,timeout=110);raise SystemExit(r.returncode)
finally:server.shutdown();server.server_close();thread.join(3)
