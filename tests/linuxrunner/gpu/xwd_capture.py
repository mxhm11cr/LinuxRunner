"""Decode the RGB samples used by the Xorg fixture, including packed RGB24.

XGetImage may return different storage formats for the root and GLX visuals.
Compare RGB values, never assume the first capture's bytes/pixel or padding.
"""
import struct

def samples(path):
    data = path.read_bytes()
    h = struct.unpack_from('>25I', data)
    header, version, fmt, depth, width, height = h[:6]
    order, bpp, pitch = h[7], h[11], h[12]
    masks = h[14:17]
    start = header + 12 * h[19]
    if (version != 7 or fmt != 2 or bpp not in (24, 32) or order not in (0, 1)
            or not 0 < depth <= bpp or not 0 < width * height <= 64 * 1024 * 1024
            or not width or not height or header < 100 or start > len(data)
            or pitch < width * (bpp // 8) or start + pitch * height > len(data)):
        raise ValueError('Unsupported or truncated XWD')
    channels = []
    used = 0
    for mask in masks:
        if not mask or mask & used or mask >= 1 << bpp:
            raise ValueError('Invalid RGB masks')
        used |= mask
        shift = (mask & -mask).bit_length() - 1
        maximum = mask >> shift
        if maximum & (maximum + 1):
            raise ValueError('Non-contiguous RGB mask')
        channels.append((mask, shift, maximum))
    def pixel(x, y):
        offset = start + y * pitch + x * (bpp // 8)
        value = int.from_bytes(data[offset:offset + bpp // 8], 'little' if order == 0 else 'big')
        return tuple(((value & mask) >> shift) * 255 // maximum for mask, shift, maximum in channels)
    return width, height, pixel

def red_root_capture(path):
    try:
        w, h, pixel = samples(path)
        return all(pixel(x, y) == (255, 0, 0) for x, y in (
            (w//2, h//2), (w//10, h//10), (9*w//10, h//10),
            (w//10, 9*h//10), (9*w//10, 9*h//10)))
    except (OSError, struct.error, ValueError):
        return False

def different_window(base, image):
    try:
        w, h, before = samples(base)
        aw, ah, after = samples(image)
        if (w, h) != (aw, ah):
            return False
        changed = total = 0
        for y in range(0, h, 8):
            for x in range(0, w, 8):
                total += 1
                changed += before(x, y) != after(x, y)
        return changed * 100 >= total
    except (OSError, struct.error, ValueError):
        return False
