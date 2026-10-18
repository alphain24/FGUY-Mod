"""Research: where an enemy keeps its health - the enemies' objects (and the objects they point to, one step) read
twice some seconds apart while they get hurt; fields that went down (a positive float, under 10000) on more than one
enemy, by where they are ("+off" in the enemy, "[+ptr]+off" behind a pointer of it).

  npchealth.py <pid> [<seconds>]
"""
import collections
import os
import struct
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from findobj import open_reader  # noqa: E402
from npcs import npcs  # noqa: E402
from pe import PE  # noqa: E402

SIZE, SUB = 0x1000, 0x300


def snap(read, a):
    out = {}
    b = read(a, SIZE)
    if not b:
        return out
    for o in range(0, SIZE, 4):
        out['+%x' % o] = b[o:o + 4]
    for o in range(0, SIZE, 4):
        p = struct.unpack_from('<I', b, o)[0]
        if not 0x10000 < p < 0x7fff0000:
            continue
        c = read(p, SUB)
        if not c:
            continue
        for k in range(0, SUB, 4):
            out['[+%x]+%x' % (o, k)] = c[k:k + 4]
    return out


def main():
    pid = int(sys.argv[1])
    wait = float(sys.argv[2]) if len(sys.argv) > 2 else 6
    exe = PE(os.environ.get('FGUY_EXE', 'A:/Games/FGUY-Research/FGUY.exe'))
    h, read = open_reader(pid)
    lst = npcs(pid, exe)
    before = {a: snap(read, a) for _, a, *_ in lst}
    time.sleep(wait)
    hits = collections.Counter()
    sample = {}
    for a, s0 in before.items():
        s1 = snap(read, a)
        for k, v0 in s0.items():
            v1 = s1.get(k)
            if v1 is None or v0 == v1:
                continue
            f0, f1 = struct.unpack('<f', v0)[0], struct.unpack('<f', v1)[0]
            if 0 <= f1 < f0 <= 10000 and f0 - f1 >= 1:
                hits[k] += 1
                sample.setdefault(k, (f0, f1))
    for k, n in hits.most_common(25):
        if n >= 2:
            print('%-16s down on %d enemies (e.g. %.1f -> %.1f)' % (k, n, *sample[k]))


if __name__ == '__main__':
    main()
