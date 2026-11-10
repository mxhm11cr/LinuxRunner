#!/usr/bin/env python3
"""Native fakefs import/export must preserve binary guest capability metadata.

Uses real libarchive PAX headers and real inode attributes. Never sets a host
security.capability attribute. Run with a newly built fakefsify executable.
"""
import argparse
import ctypes
import ctypes.util
import os
from pathlib import Path
import struct
import subprocess
import tempfile

p=argparse.ArgumentParser();p.add_argument('--fakefsify',type=Path,required=True);args=p.parse_args()
lib=ctypes.CDLL(ctypes.util.find_library('archive'))
def bind(name,ret,types):
    f=getattr(lib,name);f.restype=ret;f.argtypes=types;return f
ptr=ctypes.c_void_p; num=ctypes.c_int; cstr=ctypes.c_char_p; size=ctypes.c_size_t
new=bind('archive_write_new',ptr,[]);pax=bind('archive_write_set_format_pax',num,[ptr]);openfile=bind('archive_write_open_filename',num,[ptr,cstr]);header=bind('archive_write_header',num,[ptr,ptr]);data=bind('archive_write_data',ctypes.c_ssize_t,[ptr,ptr,size]);close=bind('archive_write_close',num,[ptr]);free=bind('archive_write_free',num,[ptr]);entry=bind('archive_entry_new',ptr,[]);entryfree=bind('archive_entry_free',None,[ptr]);path=bind('archive_entry_set_pathname',None,[ptr,cstr]);mode=bind('archive_entry_set_mode',None,[ptr,ctypes.c_uint]);length=bind('archive_entry_set_size',None,[ptr,ctypes.c_int64]);xattr=bind('archive_entry_xattr_add_entry',None,[ptr,cstr,ptr,size]);link=bind('archive_entry_set_hardlink',None,[ptr,cstr])
caps=struct.pack('<IIIII',0x02000001,(1<<10)|(1<<12)|(1<<23),0,0,0)
name='com.linuxrunner.guest.capability' if os.uname().sysname=='Darwin' else 'user.linuxrunner.capability'
native=ctypes.CDLL(None,use_errno=True)
is_mac=os.uname().sysname=='Darwin'
getattr_fd=native.fgetxattr;getattr_fd.restype=ctypes.c_ssize_t
getattr_fd.argtypes=[num,cstr,ptr,size]+([ctypes.c_uint32,num] if is_mac else [])
listattr_fd=native.flistxattr;listattr_fd.restype=ctypes.c_ssize_t
listattr_fd.argtypes=[num,ptr,size]+([num] if is_mac else [])
def get_attribute(path,attribute):
    fd=os.open(path,os.O_RDONLY);buffer=ctypes.create_string_buffer(1024)
    try:
        n=getattr_fd(fd,os.fsencode(attribute),buffer,len(buffer),*([0,0] if is_mac else []))
        if n<0:raise OSError(ctypes.get_errno(),'native fgetxattr')
        return buffer.raw[:n]
    finally:os.close(fd)
def list_attributes(path):
    fd=os.open(path,os.O_RDONLY);buffer=ctypes.create_string_buffer(4096)
    try:
        n=listattr_fd(fd,buffer,len(buffer),*([0] if is_mac else []))
        if n<0:raise OSError(ctypes.get_errno(),'native flistxattr')
        return buffer.raw[:n].decode().split('\0')
    finally:os.close(fd)
def archive(file,items):
    a=new();assert pax(a)==0 and openfile(a,os.fsencode(file))==0
    for filename,body,attribute,hardlink in items:
        e=entry();path(e,os.fsencode(filename));mode(e,0o100755);length(e,len(body))
        if attribute is not None:xattr(e,b'security.capability',attribute,len(attribute))
        if hardlink:link(e,os.fsencode(hardlink))
        assert header(a,e)==0
        if body:assert data(a,body,len(body))==len(body)
        entryfree(e)
    assert close(a)==0;free(a)
def call(*params,expect=0):
    r=subprocess.run([str(args.fakefsify.resolve()),*map(str,params)],capture_output=True,text=True)
    assert (r.returncode==0)==(expect==0),(r.returncode,r.stderr)
with tempfile.TemporaryDirectory(prefix='lr-filecap-archive-') as temporary:
    d=Path(temporary);initial=d/'initial.tar';archive(initial,[('program',b'contents',caps,None),('link',b'',None,'program')])
    root=d/'root';call(initial,root);assert get_attribute(root/'data/program',name)==caps
    assert os.stat(root/'data/program').st_ino==os.stat(root/'data/link').st_ino
    exporter=d/'unfakefsify';exporter.symlink_to(args.fakefsify.resolve())
    r=subprocess.run([str(exporter),str(root),str(d/'export.tar')],capture_output=True,text=True);assert r.returncode==0,r.stderr
    restored=d/'restored';call(d/'export.tar',restored)
    for n in ['program','link']:
        assert (restored/'data'/n).read_bytes()==b'contents'
        assert get_attribute(restored/'data'/n,name)==caps
        assert 'security.capability' not in list_attributes(restored/'data'/n)
    duplicate=d/'duplicate.tar';archive(duplicate,[('program',b'old',caps,None),('program',b'replacement',None,None)])
    dup=d/'duplicate';call(duplicate,dup);assert (dup/'data/program').read_bytes()==b'replacement'
    assert name not in list_attributes(dup/'data/program')
    malformed=d/'malformed.tar';archive(malformed,[('bad',b'contents',b'broken',None)]);call(malformed,d/'bad',expect=1)
    unsupported=d/'namespaced.tar';archive(unsupported,[('bad',b'contents',struct.pack('<IIIIII',0x03000001,1,0,0,0,1001),None)]);call(unsupported,d/'namespaced',expect=1)
print('FILECAP_ARCHIVE_PASS binary PAX, hardlink, export/import, duplicate replacement, malformed and unsupported metadata refusal; no host privileges')
