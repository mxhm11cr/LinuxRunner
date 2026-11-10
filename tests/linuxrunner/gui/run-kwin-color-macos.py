#!/usr/bin/env python3
"""KWin color regression in a disposable Mac ARM64 guest; not full Plasma/iOS.

Requires the existing virgl-enabled CLI, preserved host GPU dylibs, a guest with
KWin/weston-simple-shm/Qt/DBus development tools, and an existing non-root user.
The lease serializes this test with other builds and fixture operations.
"""
import argparse
import fcntl
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import struct
import subprocess
import zlib


def png(path, data, width, height):
    rows = bytearray()
    for y in range(height):
        rows.append(0)
        for x in range(width):
            i = (y * width + x) * 4
            rows.extend(data[i:i+3][::-1])
    def chunk(kind, value):
        return struct.pack('>I', len(value)) + kind + value + struct.pack('>I', zlib.crc32(kind + value) & 0xffffffff)
    path.write_bytes(b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', width, height, 8, 2, 0, 0, 0)) + chunk(b'IDAT', zlib.compress(rows)) + chunk(b'IEND', b''))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for key in ('runtime', 'rootfs', 'output', 'lease', 'dylib-path'):
        parser.add_argument('--' + key, type=Path, required=True)
    parser.add_argument('--guest-user', required=True)
    args = parser.parse_args()
    if not re.fullmatch(r'[a-z_][a-z0-9_-]*', args.guest_user):
        parser.error('invalid guest user')
    out = args.output.absolute()
    if ',' in str(out) or '\n' in str(out):
        parser.error('CLI bind paths cannot contain commas/newlines')
    out.mkdir(parents=True, exist_ok=False)
    exchange = out / 'exchange'
    exchange.mkdir()
    (exchange / 'logs').mkdir()
    source = Path(__file__).resolve().parent
    shutil.copy2(source / 'kwin-screencast/kwin_screenshot_capture.c', exchange / 'shot.c')
    shutil.copy2(source / 'qt_matrix_inverse_contract.cpp', exchange / 'matrix.cpp')
    (exchange / 'org.linuxrunner.kwin-shot.desktop').write_text('[Desktop Entry]\nType=Application\nName=LinuxRunner screenshot regression\nExec=/home/mobile/Downloads/kwin-shot\nNoDisplay=true\nX-KDE-DBUS-Restricted-Interfaces=org.kde.KWin.ScreenShot2\n')
    (exchange / 'scene.sh').write_text('''#!/bin/sh
set -eu
logs=/home/mobile/Downloads/logs
weston-simple-shm > "$logs/client.log" 2>&1 & client=$!
trap 'kill "$client" 2>/dev/null || true' EXIT
sleep 12
kill -0 "$client"
/home/mobile/Downloads/kwin-shot "$logs/workspace.raw" > "$logs/shot.log" 2>&1
''')
    (exchange / 'launch.sh').write_text('''#!/bin/sh
set -eu
work=$(mktemp -d /tmp/lr-kwin-color.XXXXXX)
trap 'rm -rf "$work"' EXIT
export XDG_RUNTIME_DIR="$work" XDG_CONFIG_HOME="$work/config" XDG_DATA_HOME="$work/data" XDG_CACHE_HOME="$work/cache"
mkdir -p "$XDG_CONFIG_HOME" "$XDG_DATA_HOME/applications" "$XDG_CACHE_HOME"
cp /home/mobile/Downloads/org.linuxrunner.kwin-shot.desktop "$XDG_DATA_HOME/applications/"
export XDG_CURRENT_DESKTOP=KDE XDG_SESSION_TYPE=wayland QT_QUICK_BACKEND=software QSG_RENDER_LOOP=basic
export KWIN_RENDER_NODES=/dev/dri/renderD129 LIBGL_ALWAYS_SOFTWARE=0 QT_FORCE_STDERR_LOGGING=1
unset DISPLAY WAYLAND_DISPLAY QT_QPA_PLATFORM
exec dbus-run-session -- kwin_wayland --virtual --width 960 --height 540 --socket lr-color --no-lockscreen --exit-with-session /home/mobile/Downloads/scene.sh
''')
    setup = '''set -eu
user=$1
[ "$(id -u "$user")" -gt 0 ]
cd /home/mobile/Downloads
g++ -O2 -fPIC matrix.cpp $(pkg-config --cflags --libs Qt6Gui) -o qt-matrix
./qt-matrix
gcc -O2 shot.c $(pkg-config --cflags --libs dbus-1) -o kwin-shot
chmod 755 kwin-shot launch.sh scene.sh
# Fresh host exchange paths may retain guest metadata from an earlier bind.
touch logs/client.log logs/shot.log logs/workspace.raw
chown -R "$user" logs
chmod 777 /home/mobile/Downloads logs
mkdir -p /dev/dri
for node in card1:1 renderD129:129; do n=${node%:*}; test -c /dev/dri/$n || mknod /dev/dri/$n c 226 ${node#*:}; chown "$user" /dev/dri/$n; chmod 600 /dev/dri/$n; done
test -c /dev/udmabuf || mknod /dev/udmabuf c 10 254
chown "$user" /dev/udmabuf; chmod 600 /dev/udmabuf
exec su -l -s /bin/sh -c /home/mobile/Downloads/launch.sh "$user"
'''
    env = dict(os.environ, ISH_VIRGL='1', ISH_SOFTWARE_DRM='1', ISH_SOFTWARE_DMABUF='1',
               ISH_BIND_MOUNTS='/home/mobile/Downloads=' + str(exchange),
               DYLD_LIBRARY_PATH=str(args.dylib_path) + (':' + os.environ['DYLD_LIBRARY_PATH'] if os.environ.get('DYLD_LIBRARY_PATH') else ''))
    with args.lease.open('a') as lease:
        fcntl.flock(lease, fcntl.LOCK_EX)
        with (out / 'guest.log').open('w') as log:
            try:
                result = subprocess.run([str(args.runtime.resolve()), '-f', str(args.rootfs.resolve()), '/bin/sh', '-ec', setup,
                                         'kwin-color-test', args.guest_user], env=env, stdout=log, stderr=subprocess.STDOUT, timeout=180)
                code = result.returncode
            except subprocess.TimeoutExpired:
                code = 124
    raw = exchange / 'logs/workspace.raw'
    data = raw.read_bytes() if raw.exists() else b''
    colors = {data[i:i+3] for i in range(0, len(data)-3, 4)}
    valid = len(data) == 960 * 540 * 4
    passed = code == 0 and valid and len(colors) > 100
    report = dict(status='PASS' if passed else 'FAIL', exit=code, bytes=len(data), distinct_rgb=len(colors),
                  runtime_sha256=hashlib.sha256(args.runtime.read_bytes()).hexdigest(),
                  scope='Mac interpreter, KWin virtual backend, actual wl_shm client and workspace capture; not full Plasma or physical iOS')
    if valid:
        png(out / 'workspace.png', data, 960, 540)
    (out / 'result.json').write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps(report, indent=2))
    return 0 if passed else 1


if __name__ == '__main__':
    raise SystemExit(main())
