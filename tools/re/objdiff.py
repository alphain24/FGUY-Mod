"""Research: the objects of a class in two running games side by side, dword by dword (as hex and as a float where it
looks like one), only where they differ - to see which fields each PC keeps differently (one object of the class
each, or the first).

  objdiff.py <pid a> <pid b> <vtable hex> [<bytes, default 0x200>] [all]   (all: the equal ones too)
"""
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from findobj import open_reader  # noqa: E402
from vtcount import count  # noqa: E402


def looks_float(v):
    f = struct.unpack('<f', struct.pack('<I', v))[0]
    return v != 0 and 1e-4 < abs(f) < 1e7


def main():
    pa, pb, vt = int(sys.argv[1]), int(sys.argv[2]), int(sys.argv[3], 16)
    size = int(sys.argv[4], 0) if len(sys.argv) > 4 else 0x200
    every = 'all' in sys.argv[5:]
    rows = []
    for pid in (pa, pb):
        h, r = open_reader(pid)
        objs = count(h, r).get(vt, [])
        if not objs:
            print('%d: none' % pid)
            return
        rows.append((objs[0], r(objs[0], size)))
    (aa, ba), (ab, bb) = rows
    print('%d: %08x   %d: %08x' % (pa, aa, pb, ab))
    for o in range(0, size, 4):
        x, y = struct.unpack_from('<I', ba, o)[0], struct.unpack_from('<I', bb, o)[0]
        if x == y and not every:
            continue
        fx = '%12.3f' % struct.unpack('<f', struct.pack('<I', x))[0] if looks_float(x) else ' ' * 12
        fy = '%12.3f' % struct.unpack('<f', struct.pack('<I', y))[0] if looks_float(y) else ' ' * 12
        print('+%03x  %08x %s   %08x %s%s' % (o, x, fx, y, fy, '' if x != y else '   (same)'))


if __name__ == '__main__':
    main()
