#!/usr/bin/env python3
"""Real application CLI operations in an explicitly disposable fakefs fixture.
GUI launch, input and rendering are separate tests. No app security bypasses.
"""
import argparse,pathlib,os,subprocess,fcntl,json,time,uuid
p=argparse.ArgumentParser(description=__doc__)
for key in ('runtime','rootfs','exchange','output'):p.add_argument('--'+key,required=True)
p.add_argument('--disposable',action='store_true',required=True,help='Acknowledge that only a disposable rootfs is supplied')
p.add_argument('--app',action='append',help='Run only named rows; repeatable')
a=p.parse_args();r=pathlib.Path(a.rootfs).resolve();ex=pathlib.Path(a.exchange).resolve();ex.mkdir(parents=True,exist_ok=True)
o=pathlib.Path(a.output).resolve()
if o.exists() and any(o.iterdir()):p.error('Use a new, empty output directory')
o.mkdir(parents=True,exist_ok=True);runtime=pathlib.Path(a.runtime).resolve()
env=dict(os.environ,ISH_NETLINK_STUB='1',ISH_BIND_MOUNTS='/home/mobile/Downloads='+str(ex))
scratch='/tmp/linuxrunner-apps-'+uuid.uuid4().hex
checks=[
('python','python3 -c \'import json,pathlib,hashlib; p=pathlib.Path("python.json"); p.write_text(json.dumps({"value":sum(range(10000))})); assert json.loads(p.read_text())["value"]==49995000; print("PYTHON_IO_PASS")\'', 'PYTHON_IO_PASS'),
('node','node -e \'const fs=require("fs");fs.writeFileSync("node.json",JSON.stringify({answer:6*7}));if(JSON.parse(fs.readFileSync("node.json")).answer!==42)process.exit(1); console.log("NODE_IO_PASS")\'','NODE_IO_PASS'),
('npm','command -v npm; printf \'{"scripts":{"test":"node -e \\\"console.log(12345)\\\""}}\' > package.json; npm run --offline test','12345'),
('git','mkdir repo; cd repo; git init -q; printf "version one\\n" > file; git add file; git -c user.name=LinuxRunnerTest -c user.email=fixture@example.invalid commit -qm fixture; test "$(git show HEAD:file)" = "version one"; printf "version two\\n" >> file; git diff --exit-code -- file && exit 1; echo GIT_EDIT_HISTORY_PASS','GIT_EDIT_HISTORY_PASS'),
('gcc','printf \'#include <stdio.h>\\nint main(void){puts("GCC_EXEC_PASS");}\\n\' > gcc-test.c; gcc -O2 gcc-test.c -o gcc-test; ./gcc-test','GCC_EXEC_PASS'),
('clang','printf \'#include <stdio.h>\\nint main(void){puts("CLANG_EXEC_PASS");}\\n\' > clang-test.c; clang -O2 clang-test.c -o clang-test; ./clang-test','CLANG_EXEC_PASS'),
('cmake','mkdir cm; cd cm; printf \'cmake_minimum_required(VERSION 3.20)\\nproject(fixture C)\\nadd_executable(fixture main.c)\\n\' > CMakeLists.txt; printf \'#include <stdio.h>\\nint main(void){puts("CMAKE_BUILD_PASS");}\\n\' > main.c; cmake -S . -B build; cmake --build build -j 1; ./build/fixture','CMAKE_BUILD_PASS'),
('blender','blender --background --factory-startup --python-expr \'import bpy; bpy.ops.wm.save_as_mainfile(filepath="/tmp/linuxrunner-cli-apps/blender-fixture.blend"); print("BLENDER_DATA_SAVE_PASS")\'','BLENDER_DATA_SAVE_PASS'),
('inkscape','printf \'<svg xmlns="http://www.w3.org/2000/svg" width="32" height="32"><rect width="32" height="32" fill="red"/></svg>\' > fixture.svg; inkscape --export-type=png --export-filename=fixture.png fixture.svg; test -s fixture.png; echo INKSCAPE_EXPORT_PASS','INKSCAPE_EXPORT_PASS'),
('libreoffice','printf "LinuxRunner PDF export fixture\\n" > document.txt; libreoffice -env:UserInstallation=file:///tmp/linuxrunner-cli-apps/lo-profile --headless --convert-to pdf --outdir . document.txt; test -s document.pdf; echo LIBREOFFICE_PDF_PASS','LIBREOFFICE_PDF_PASS'),
('curl-https','curl --noproxy "*" --fail --max-time 15 -sS https://example.com/ -o example.html; grep -q "Example Domain" example.html; echo HTTPS_CERTIFICATE_PASS','HTTPS_CERTIFICATE_PASS'),
('wget-https','wget --no-proxy -T 15 -qO example-wget.html https://example.com/; grep -q "Example Domain" example-wget.html; echo WGET_HTTPS_PASS','WGET_HTTPS_PASS'),
('interfaces','ip addr; echo IP_ADDR_PASS; if ip route; then echo ROUTE_UNEXPECTED_SUCCESS; else echo ROUTE_UNSUPPORTED_AS_REPORTED; fi','IP_ADDR_PASS'),
('ping','ping -c 1 -W 2 127.0.0.1; echo PING_LOOPBACK_PASS','PING_LOOPBACK_PASS')]
with (r/'.host-gui-test.lock').open('a') as lease:
 fcntl.flock(lease,fcntl.LOCK_EX|fcntl.LOCK_NB)
 records=[]
 for name,body,marker in checks:
  if a.app and name not in a.app:continue
  body=body.replace('/tmp/linuxrunner-cli-apps',scratch)
  script='set -eu; mkdir -p '+scratch+'; cd '+scratch+'; '+body
  started=time.monotonic()
  with (o/('app-cli-'+name+'.log')).open('w') as log:
   try:p=subprocess.run([str(runtime),'-f',str(r),'/usr/bin/timeout','-k','3','45','/bin/sh','-ec',script],env=env,stdout=log,stderr=subprocess.STDOUT,timeout=55);code=p.returncode
   except subprocess.TimeoutExpired:code=124
  text=(o/('app-cli-'+name+'.log')).read_text(errors='replace');record=dict(application=name,status='PASS' if code==0 and marker in text else 'FAIL',exit=code,seconds=time.monotonic()-started,scope='ARM64 LinuxRunner interpreter fixture; CLI operation only',marker=marker in text,guest_workdir=scratch)
  records.append(record);(o/'app-cli-summary.json').write_text(json.dumps(records,indent=2)+'\n');print(record,flush=True)
