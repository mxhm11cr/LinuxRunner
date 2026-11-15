#!/usr/bin/env python3
# Fake RFB server for rfb_cursor_hotspot_regress: after the handshake it sends
# one FramebufferUpdate with a 1x1 cursor whose hotspot (11,11) lies outside
# the image (x11vnc's cropped hidden cursor while GNOME hides the pointer
# during typing), followed by a 16x16 raw rectangle. A correct client keeps
# the connection and delivers the frame.
import socket, struct, sys
s = socket.socket(); s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
s.bind(("127.0.0.1", 0)); s.listen(1)
print(s.getsockname()[1], flush=True)
c, _ = s.accept()
def recv(n):
    b = b""
    while len(b) < n:
        part = c.recv(n - len(b))
        if not part: sys.exit(0)
        b += part
    return b
c.sendall(b"RFB 003.008\n"); recv(12)
c.sendall(bytes([1, 2])); recv(1)
c.sendall(b"\0" * 16); recv(16)
c.sendall(struct.pack(">I", 0)); recv(1)
pf = struct.pack(">BBBBHHHBBBxxx", 32, 24, 0, 1, 255, 255, 255, 16, 8, 0)
c.sendall(struct.pack(">HH", 16, 16) + pf + struct.pack(">I", 4) + b"test")
c.settimeout(2)
try:
    while True: c.recv(4096)          # SetPixelFormat / SetEncodings / requests
except socket.timeout: pass
if len(sys.argv) > 1 and sys.argv[1] == "copyrect":
    # x11vnc moving a window: a 16x16 frame whose bottom-right quarter is
    # B=(0x11,0x22,0x33), then CopyRect of that quarter to the top-left.
    pixels = b"".join((b"\x33\x22\x11\x00" if (x >= 8 and y >= 8) else b"\x40\x80\xc0\x00") for y in range(16) for x in range(16))
    c.sendall(struct.pack(">BxH", 0, 1) + struct.pack(">HHHHi", 0, 0, 16, 16, 0) + pixels)
    try:
        while True: c.recv(4096)
    except socket.timeout: pass
    c.sendall(struct.pack(">BxH", 0, 1) + struct.pack(">HHHHi", 0, 0, 8, 8, 1) + struct.pack(">HH", 8, 8))
elif len(sys.argv) > 1 and sys.argv[1] == "grow":
    # The X screen grew to 32x16: x11vnc sends pixels of the new size first,
    # then its NewFBSize rectangle.
    raw = struct.pack(">HHHHi", 0, 0, 32, 16, 0) + b"\x40\x80\xc0\x00" * 512
    newsize = struct.pack(">HHHHi", 0, 0, 32, 16, -223)
    c.sendall(struct.pack(">BxH", 0, 2) + raw + newsize)
else:
    cursor = struct.pack(">HHHHi", 11, 11, 1, 1, -239) + b"\x10\x20\x30\x00" + b"\x80"
    raw = struct.pack(">HHHHi", 0, 0, 16, 16, 0) + b"\x40\x80\xc0\x00" * 256
    c.sendall(struct.pack(">BxH", 0, 2) + cursor + raw)
c.settimeout(5)
try:
    while True:
        if not c.recv(4096): break
except Exception: pass
