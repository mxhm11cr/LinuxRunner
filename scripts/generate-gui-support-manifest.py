#!/usr/bin/env python3
"""Canonical IPA resource list and checksums, independent of app build number."""
import hashlib,json,pathlib,sys
bundle=pathlib.Path(__file__).resolve().parents[1]/'app/LinuxRunner/GUISupport.bundle'
names=sorted(p.name for p in bundle.iterdir() if p.is_file() and p.name not in ('support-manifest.json','support.sha256'))
resources=[dict(name=n,sha256=hashlib.sha256((bundle/n).read_bytes()).hexdigest(),mode='0755' if n.endswith('.sh') or n=='capture-shm-guard.arm64' else '0644') for n in names]
manifest=json.dumps(dict(schema=1,version=(bundle/'support-version').read_text().strip(),resources=resources),indent=2)+'\n'
sums=''.join(f"{r['sha256']}  {r['name']}\n" for r in resources)
for name,content in [('support-manifest.json',manifest),('support.sha256',sums)]:
 if '--check' in sys.argv:
  assert (bundle/name).read_text()==content,f'{name} stale; run {__file__}'
 else:(bundle/name).write_text(content)
print('GUI support manifest:',len(resources),'resources; verified' if '--check' in sys.argv else 'generated')
