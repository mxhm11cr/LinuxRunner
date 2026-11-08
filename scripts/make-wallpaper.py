#!/usr/bin/env python3
# LinuxRunner desktop wallpaper, drawn procedurally (no image sources): a deep
# Nord gradient with soft aurora ribbons, centred so that both portrait and
# landscape crops ("fill") keep the composition. Ordered dithering avoids
# banding in the dark gradient. Pure Python: math + zlib.
#   scripts/make-wallpaper.py OUTPUT.png [WIDTH HEIGHT]
import math, struct, sys, zlib

out = sys.argv[1]
W, H = (int(sys.argv[2]), int(sys.argv[3])) if len(sys.argv) > 3 else (1280, 1280)

def mix(a, b, t):
    return tuple(x + (y - x) * t for x, y in zip(a, b))

top, bottom = (0x1b, 0x20, 0x2a), (0x0c, 0x10, 0x18)      # Nord night, darker than polar night
ribbons = [  # (colour, centre height, amplitude, wavelength, phase, thickness, strength)
    ((0x88, 0xc0, 0xd0), 0.56, 0.06, 1.25, 0.3, 0.035, 0.75),   # frost
    ((0x5e, 0x81, 0xac), 0.61, 0.08, 1.60, 1.9, 0.060, 0.45),   # blue
    ((0xb4, 0x8e, 0xad), 0.66, 0.05, 2.10, 4.1, 0.030, 0.32),   # aurora purple
    ((0xa3, 0xbe, 0x8c), 0.53, 0.04, 0.95, 2.6, 0.020, 0.18),   # aurora green
]
bayer = [[0, 8, 2, 10], [12, 4, 14, 6], [3, 11, 1, 9], [15, 7, 13, 5]]
rows = []
for y in range(H):
    v = y / (H - 1)
    base = mix(top, bottom, v ** 1.3)
    row = bytearray(b'\0')
    for x in range(W):
        u = x / (W - 1)
        r, g, b = base
        for col, c, amp, wl, ph, th, st in ribbons:
            centre = c + amp * math.sin(2 * math.pi * u / wl + ph) + 0.015 * math.sin(7 * u + ph * 3)
            d = (v - centre) / th
            glow = math.exp(-d * d) * st * (0.55 + 0.45 * math.sin(math.pi * u * 0.9 + ph))
            # A faint curtain above each ribbon, as aurorae fade upwards.
            if v < centre:
                glow += math.exp(-((centre - v) / (th * 6)) ** 2) * st * 0.18
            r += (col[0] - r) * glow * 0.8; g += (col[1] - g) * glow * 0.8; b += (col[2] - b) * glow * 0.8
        # Vignette towards the corners.
        k = 1 - 0.35 * (((u - 0.5) * 1.2) ** 2 + ((v - 0.45) * 1.1) ** 2)
        dither = (bayer[y & 3][x & 3] - 7.5) / 16
        row += bytes(max(0, min(255, int(ch * k + dither + 0.5))) for ch in (r, g, b))
    rows.append(bytes(row))

def chunk(kind, data):
    return struct.pack('>I', len(data)) + kind + data + struct.pack('>I', zlib.crc32(kind + data) & 0xffffffff)

png = b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', W, H, 8, 2, 0, 0, 0))
png += chunk(b'IDAT', zlib.compress(b''.join(rows), 9)) + chunk(b'IEND', b'')
open(out, 'wb').write(png)
print(out, W, H, len(png), 'bytes')
