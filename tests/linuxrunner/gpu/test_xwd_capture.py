import struct
import tempfile
import unittest
from pathlib import Path
from xwd_capture import different_window, red_root_capture

def image(path, bpp, order=0, changed=False, padding=0):
    w = h = 64
    pitch = w * (bpp // 8) + padding
    header = [100, 7, 2, 24, w, h, 0, order, 32, order, 8, bpp, pitch,
              4, 0xff0000, 0xff00, 0xff, 8, 256, 0, w, h, 0, 0, 0]
    data = bytearray(struct.pack('>25I', *header))
    for y in range(h):
        for x in range(w):
            rgb = 0x00ff00 if changed and x < 32 and y < 32 else 0xff0000
            data.extend(rgb.to_bytes(bpp // 8, 'little' if order == 0 else 'big'))
        data.extend(b'\xa5' * padding)
    path.write_bytes(data)

class XWDCaptureTest(unittest.TestCase):
    def test_storage_change_does_not_hide_window(self):
        with tempfile.TemporaryDirectory() as root:
            base, frame = Path(root)/'base', Path(root)/'frame'
            image(base, 32)
            for bpp in (24, 32):
                for order in (0, 1):
                    image(frame, bpp, order, padding=64)
                    self.assertTrue(red_root_capture(frame))
                    self.assertFalse(different_window(base, frame))
                    image(frame, bpp, order, changed=True, padding=64)
                    self.assertTrue(different_window(base, frame))

    def test_truncated_and_bad_format_are_rejected(self):
        with tempfile.TemporaryDirectory() as root:
            base, frame = Path(root)/'base', Path(root)/'frame'
            image(base, 32)
            frame.write_bytes(base.read_bytes()[:-20])
            self.assertFalse(different_window(base, frame))
            self.assertFalse(red_root_capture(frame))

if __name__ == '__main__':
    unittest.main()
