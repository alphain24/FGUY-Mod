"""Research: what objects appeared (or went) in a running game - every object in its heap counted by its class (the
vtable: a first dword in .rdata whose first entry is code), saved, then compared.

  vtcount.py <pid> snap <file>             count now, save (the objects' addresses too)
  vtcount.py <pid> diff <file> [<max>]     count now and print the classes whose count changed, with the new objects'
                                            addresses (up to <max> each, default 6)
"""
import os
import pickle
import sys

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from findobj import open_reader, regions  # noqa: E402

RDATA = (0x9dc000, 0xac1000)
TEXT = (0x401000, 0x9dc000)
IMAGE = (0x400000, 0xc51000)


def count(h, read):
    rd = read(RDATA[0], RDATA[1] - RDATA[0])
    rdw = np.frombuffer(rd[:len(rd) // 4 * 4], dtype='<u4')
    first = rdw  # rdata dword i is the first entry of a vtable at RDATA[0] + 4i if it points into .text
    is_vt = (first >= TEXT[0]) & (first < TEXT[1])
    objs = {}
    for base, size in regions(h):
        if IMAGE[0] <= base < IMAGE[1]:
            continue
        b = read(base, size)
        if not b:
            continue
        w = np.frombuffer(b[:len(b) // 4 * 4], dtype='<u4')
        m = (w >= RDATA[0]) & (w < RDATA[1]) & ((w & 3) == 0)
        idx = np.nonzero(m)[0]
        if not len(idx):
            continue
        vals = w[idx]
        ok = is_vt[(vals - RDATA[0]) >> 2]
        for i, v in zip(idx[ok], vals[ok]):
            objs.setdefault(int(v), []).append(base + 4 * int(i))
    return objs


def main():
    pid, cmd, path = int(sys.argv[1]), sys.argv[2], sys.argv[3]
    h, read = open_reader(pid)
    now = count(h, read)
    if cmd == 'snap':
        pickle.dump(now, open(path, 'wb'))
        print('%d classes, %d objects' % (len(now), sum(len(v) for v in now.values())))
        return
    most = int(sys.argv[4]) if len(sys.argv) > 4 else 6
    was = pickle.load(open(path, 'rb'))
    for vt in sorted(set(now) | set(was)):
        a, b = set(was.get(vt, ())), set(now.get(vt, ()))
        if len(a) != len(b):
            new = sorted(b - a)
            print('%08x: %d -> %d  new %s' % (vt, len(a), len(b), ' '.join('%08x' % x for x in new[:most])))


if __name__ == '__main__':
    main()
