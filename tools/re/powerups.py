"""Research: the pickup spots that can show more than one kind (the power-ups?) in a running level, with where they are,
which kind is switched on, and each kind's asset (its first dwords, and any text it points to).

  powerups.py <pid> scan <out.pkl>     scan the memory for every zFloatingCollectible (a few minutes), save and print
  powerups.py <pid> show <in.pkl>      print again from the saved objects
"""
import collections
import os
import pickle
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from findobj import open_reader, regions  # noqa: E402

VT = 0xa8deb4


def text(read, a):
    b = read(a, 48) if a else None
    if not b:
        return None
    s = b.split(b'\0')[0]
    return s.decode('latin1') if len(s) >= 4 and all(32 <= c < 127 for c in s) else None


def pos(read, obj):
    m = struct.unpack('<I', read(obj + 0x38, 4))[0]
    b = read(m + 0x30, 12) if m else None
    return struct.unpack('<3f', b) if b else None


def show(read, objs):
    spots = collections.defaultdict(list)
    for a in objs:
        b = read(a, 0xb0)
        if b and struct.unpack_from('<I', b, 0)[0] == VT:
            spots[struct.unpack_from('<I', b, 0x18)[0]].append((a, b))
    for sid in sorted(spots):
        kinds = spots[sid]
        if len(kinds) < 2:
            continue
        p = pos(read, kinds[0][0])
        print('spot %#x at %s' % (sid, '(%.1f %.1f %.1f)' % p if p else '?'))
        for a, b in kinds:
            asset = struct.unpack_from('<I', b, 0x68)[0]
            ab = read(asset, 0x40) if asset else None
            words = struct.unpack('<16I', ab) if ab and len(ab) == 0x40 else ()
            names = [t for t in (text(read, w) for w in words) if t]
            print('  %08x kind %-6x on %d showing %d taken %d  %s  %s' % (
                a, words[0] if words else 0, b[0xa1], b[0xa3], b[0x75],
                ' '.join('%x' % w for w in words[:8]), names))


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
