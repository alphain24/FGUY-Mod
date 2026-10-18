"""Research: each PC's enemies' facing side by side by id (transform yaw = atan2(forward x, z), and the steering's
heading +0xc0), for the ones standing within 3 of each other (the same one).

  npcfacing.py <host pid> <other pid> [times]
"""
import math
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from findobj import open_reader  # noqa: E402
from npcs import npcs  # noqa: E402
from pe import PE  # noqa: E402


def state(pid, exe):
    h, r = open_reader(pid)
    ptr = lambda x: struct.unpack('<I', r(x, 4))[0]
    out = {}
    for e in npcs(pid, exe):
        a = e[1]
        row = r(ptr(a + 0x38) + 0x20, 12)
        if not row:
            continue
        f = struct.unpack('<3f', row)
        steer = r(ptr(a + 0x418) + 0xc0, 4)
        out[e[2]] = (math.degrees(math.atan2(f[0], f[2])),
                     math.degrees(struct.unpack('<f', steer)[0]) if steer else float('nan'), e[6])
    return out


def main():
    exe = PE(os.environ.get('FGUY_EXE', 'A:/Games/FGUY-Research/FGUY.exe'))
    for _ in range(int(sys.argv[3]) if len(sys.argv) > 3 else 1):
        a, b = state(int(sys.argv[1]), exe), state(int(sys.argv[2]), exe)
        offs = []
        for k in sorted(set(a) & set(b)):
            d = math.dist(a[k][2], b[k][2])
            if d < 3:
                off = (b[k][0] - a[k][0] + 540) % 360 - 180
                offs.append(abs(off))
                print('%x host yaw %7.1f steer %7.1f | other yaw %7.1f steer %7.1f  %6.1f deg off  (%.1f apart)' % (
                    k, a[k][0], a[k][1], b[k][0], b[k][1], off, d))
        if offs:
            print('  %d enemies: %.1f deg off on average, median %.1f' % (len(offs), sum(offs) / len(offs),
                                                                          sorted(offs)[len(offs) // 2]))
        print()


if __name__ == '__main__':
    main()
