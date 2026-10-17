"""Research: a running level's pickups (zFloatingCollectible, vtable 0xa8deb4) that are out to be taken (byte +0xa3),
with their positions and ids (+0x18), saved for pickuptest.py. Scans the game's memory (read only, a few minutes).

  pickups.py <pid> <out.pkl>
"""
import os
import pickle
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from findobj import open_reader, regions  # noqa: E402

VT = 0xa8deb4


def main():
    pid = int(sys.argv[1])
    h, read = open_reader(pid)
    found = []
    pat = struct.pack('<I', VT)
    for base, size in regions(h):
        b = read(base, size)
        if not b:
            continue
        i = b.find(pat)
        while i >= 0:
            if i % 4 == 0:
                found.append(base + i)
            i = b.find(pat, i + 1)
    active = []
    for a in found:
        b = read(a, 0xe0)
        if not b or b[0xa3] == 0:
            continue
        t = struct.unpack_from('<I', b, 0x38)[0]
        pos = struct.unpack('<3f', read(t + 0x30, 12)) if t else None
        if not pos or (abs(pos[0]) < 1 and abs(pos[2]) < 1) or abs(pos[1]) > 500:
            continue
        active.append((a, pos))
        print('%#x id %#x type %08x at (%.1f, %.1f, %.1f)' % (a, struct.unpack_from('<I', b, 0x18)[0],
                                                             struct.unpack_from('<I', b, 0x68)[0], *pos))
    pickle.dump(active, open(sys.argv[2], 'wb'))
    print('%d pickups of %d objects' % (len(active), len(found)))


if __name__ == '__main__':
    main()
