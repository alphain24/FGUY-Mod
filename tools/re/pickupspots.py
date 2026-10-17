"""Research: a running level's pickup spots - every zFloatingCollectible (vtable 0xa8deb4) grouped by its spot id (+0x18),
with each kind's state bytes (+0x75 taken, +0xa0..a3: a1 = this kind is switched on at the spot, a3 = showing).

  pickupspots.py <pid> scan <out.pkl>        scan the memory (a few minutes), save every object, print the spots
  pickupspots.py <pid> show <in.pkl>         print the spots again from the saved objects (quick: reads them only)
"""
import collections
import os
import pickle
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from findobj import open_reader, regions  # noqa: E402

VT = 0xa8deb4


def show(read, objs):
    spots = collections.defaultdict(list)
    for a in objs:
        b = read(a, 0xb0)
        if not b or struct.unpack_from('<I', b, 0)[0] != VT:
            continue
        spots[struct.unpack_from('<I', b, 0x18)[0]].append((a, b))
    for sid in sorted(spots):
        kinds = spots[sid]
        if len(kinds) < 2 and not any(b[0xa3] for _, b in kinds):
            continue
        parts = []
        for a, b in kinds:
            asset = struct.unpack_from('<I', b, 0x68)[0]
            aid = struct.unpack('<I', read(asset, 4))[0] if asset else 0
            parts.append('%x[kind %x taken %d a1 %d a2 %d a3 %d]' % (a & 0xffffff, aid, b[0x75], b[0xa1], b[0xa2], b[0xa3]))
        print('spot %#x: %s' % (sid, ' '.join(parts)))


def main():
    pid, cmd, path = int(sys.argv[1]), sys.argv[2], sys.argv[3]
    h, read = open_reader(pid)
    if cmd == 'scan':
        objs = []
        pat = struct.pack('<I', VT)
        for base, size in regions(h):
            b = read(base, size)
            if not b:
                continue
            i = b.find(pat)
            while i >= 0:
                if i % 4 == 0:
                    objs.append(base + i)
                i = b.find(pat, i + 1)
        pickle.dump(objs, open(path, 'wb'))
    else:
        objs = pickle.load(open(path, 'rb'))
    show(read, objs)


if __name__ == '__main__':
    main()
