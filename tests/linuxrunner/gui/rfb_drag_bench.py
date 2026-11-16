#!/usr/bin/env python3
"""Drag a window over RFB as the iOS app does (measurement tool).
usage: rfb_drag_bench.py <exchange-dir> <minimal|full> <out.json>
Waits for <exchange>/drag-ready ("<client x> <client y>"), connects to the
newest session's x11vnc (port/password files), presses on the title bar and
moves 60 times a second for 2 x 5 s (right, then back), like a finger drag.
'minimal' requests 1x1 incremental updates (the app with direct scanout),
'full' requests the whole screen (the app before)."""
import glob, json, os, socket, struct, subprocess, sys, threading, time
exchange, policy, out = sys.argv[1], sys.argv[2], sys.argv[3]
ready = os.environ.get('DRAG_READY') or os.path.join(exchange, 'drag-ready')
for _ in range(600):
    if os.path.exists(ready) and open(ready).read().strip(): break
    time.sleep(0.5)
else: sys.exit('no drag-ready')
cx, cy = map(int, open(ready).read().split())
session = max(glob.glob(os.path.join(exchange, '.linuxrunner/gui/session-*/port')), key=os.path.getmtime)
d = os.path.dirname(session); port = int(open(session).read()); password = open(os.path.join(d, 'password')).read().split('\n')[0][:8]
def des(key, block):
    key = bytes(int(f'{b:08b}'[::-1], 2) for b in key.encode().ljust(8, b'\0'))
    r = subprocess.run(['openssl', 'enc', '-des-ecb', '-nopad', '-K', key.hex()], input=block, capture_output=True)
    if r.returncode: r = subprocess.run(['openssl', 'enc', '-des-ecb', '-nopad', '-provider', 'legacy', '-provider', 'default', '-K', key.hex()], input=block, capture_output=True)
    return r.stdout
s = socket.create_connection(('127.0.0.1', port)); s.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
def recv(n):
    b = b''
    while len(b) < n:
        c = s.recv(n - len(b))
        if not c: raise EOFError
        b += c
    return b
recv(12); s.sendall(b'RFB 003.008\n')
types = recv(recv(1)[0]); s.sendall(bytes([2])); ch = recv(16); s.sendall(des(password, ch[:8]) + des(password, ch[8:]))
assert struct.unpack('>I', recv(4))[0] == 0, 'auth failed'
s.sendall(b'\x01'); w, h = struct.unpack('>HH', recv(4)); recv(16); recv(struct.unpack('>I', recv(4))[0])
s.sendall(struct.pack('>BxxxBBBBHHHBBBxxx', 0, 32, 24, 0, 1, 255, 255, 255, 16, 8, 0))
s.sendall(struct.pack('>BxHiii', 2, 3, 5, 0, -239))   # Hextile, Raw, RichCursor
rw, rh = (1, 1) if policy == 'minimal' else (w, h)
stats = {'updates': 0, 'bytes': 0}
def reader():
    try:
        while True:
            t = recv(1)[0]
            if t == 0:
                recv(1); n = struct.unpack('>H', recv(2))[0]
                for _ in range(n):
                    x, y, rwid, rhei, enc = struct.unpack('>HHHHi', recv(12))
                    if enc == 0: recv(rwid * rhei * 4); stats['bytes'] += rwid * rhei * 4
                    elif enc == -239: recv(rwid * rhei * 4 + ((rwid + 7) // 8) * rhei)
                    elif enc == 5:
                        for ty in range(0, rhei, 16):
                            for tx in range(0, rwid, 16):
                                tw, th = min(16, rwid - tx), min(16, rhei - ty); m = recv(1)[0]
                                if m & 1: recv(tw * th * 4); stats['bytes'] += tw * th * 4; continue
                                if m & 2: recv(4)
                                if m & 4: recv(4)
                                if m & 8:
                                    k = recv(1)[0]; recv(k * (6 if m & 16 else 2)); stats['bytes'] += k * 6
                    else: raise SystemExit(f'encoding {enc}')
                stats['updates'] += 1
                s.sendall(struct.pack('>BBHHHH', 3, 1, 0, 0, rw, rh))
            elif t == 2: pass
            elif t == 3: recv(3); recv(struct.unpack('>I', recv(4))[0])
    except (EOFError, OSError): pass
threading.Thread(target=reader, daemon=True).start()
s.sendall(struct.pack('>BBHHHH', 3, 0, 0, 0, w, h))   # first full frame, as the app does
time.sleep(3)
def pointer(mask, x, y): s.sendall(struct.pack('>BBHH', 5, mask, x, y))
x0, y0 = cx + 120, max(cy - 10, 2)   # title bar (frame above the client area)
def runtime_cpu():
    # Host CPU seconds of the LinuxRunner runtime (every guest process runs in it).
    total = 0.0
    for line in subprocess.run(['ps', '-A', '-o', 'time=,command='], capture_output=True, text=True).stdout.splitlines():
        t, _, cmd = line.strip().partition(' ')
        if '/ish' not in cmd.split(' ')[0]: continue
        parts = [float(x) for x in t.replace('-', ':').split(':')]
        secs = 0.0
        for x in parts: secs = secs * 60 + x
        total += secs
    return total
cpu_before = runtime_cpu()
pointer(0, x0, y0); time.sleep(0.3); pointer(1, x0, y0); time.sleep(0.2)
start = time.time(); marks = {'drag_start_s': None}
# DRAG_CIRCLE=1: move the window in fast circles (radius 150, ~1 per second, 4 times).
if os.environ.get('DRAG_CIRCLE') == '1':
    import math
    for k in range(4 * 60):
        a = 2 * math.pi * k / 60
        pointer(1, int(x0 + 150 * math.cos(a) - 150), int(y0 + 120 * math.sin(a)) + 120); time.sleep(1 / 60)
    step = 0
# DRAG_STEP (pixels per 60 Hz step, default 1): fast drags with large jumps.
step = int(os.environ.get('DRAG_STEP', '1')) if os.environ.get('DRAG_CIRCLE') != '1' else 0; span = 300 if step == 1 else 400
for rep in range(0 if step == 0 else 1 if step == 1 else 4):
    for direction in (1, -1):
        for i in range(0, span, step):
            dx = i + step if direction > 0 else span - (i + step)
            pointer(1, x0 - (0 if step == 1 else 200) + dx, y0 + dx // 3); time.sleep(1 / 60)
pointer(0, x0, y0); end = time.time()
cpu_drag = runtime_cpu() - cpu_before
time.sleep(2)
json.dump({'policy': policy, 'runtime_cpu_seconds_during_drag': round(cpu_drag, 2), 'drag_seconds': round(end - start, 2), 'updates': stats['updates'], 'rfb_pixel_bytes': stats['bytes'],
           'drag_start_epoch': start, 'drag_end_epoch': end}, open(out, 'w'))
print('DRAG_BENCH', json.dumps(json.load(open(out))))
