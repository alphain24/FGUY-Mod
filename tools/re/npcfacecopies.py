"""Research: where an enemy keeps which way it faces - float triples equal (within 0.02) to its transform's forward row
([npc+0x38] +0x20), and single floats equal to its yaw (atan2 of that row, either sign / convention), in the enemy
object and the objects it points to (two steps), for a few enemies: the same places for each = its facing.

  npcfacecopies.py <pid>
"""
import collections
import math
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from findobj import open_reader  # noqa: E402
from npcs import npcs  # noqa: E402
from pe import PE  # noqa: E402


def walk(read, a, fwd, yaws, depth, path, out, seen):
    b = read(a, 0x1200 if depth == 0 else 0x600)
    if not b:
        return
    for o in range(0, len(b) - 11, 4):
        v = struct.unpack_from('<3f', b, o)
        if all(abs(v[i] - fwd[i]) < 0.02 for i in range(3)):
            out.add('%s+%x fwd' % (path, o))
        elif all(abs(v[i] + fwd[i]) < 0.02 for i in range(3)):
            out.add('%s+%x -fwd' % (path, o))
    for o in range(0, len(b) - 3, 4):
        f = struct.unpack_from('<f', b, o)[0]
        for name, y in yaws:
            if abs(f - y) < 0.01:
                out.add('%s+%x %s' % (path, o, name))
    if depth >= 2:
        return
    for o in range(0, len(b) - 3, 4):
        q = struct.unpack_from('<I', b, o)[0]
        if 0x01000000 < q < 0x7fff0000 and q not in seen:
            seen.add(q)
            walk(read, q, fwd, yaws, depth + 1, '%s[+%x]' % (path, o), out, seen)


def main():
    pid = int(sys.argv[1])
    h, read = open_reader(pid)
    lst = npcs(pid, PE(os.environ.get('FGUY_EXE', 'A:/Games/FGUY-Research/FGUY.exe')))
    count = collections.Counter()
    n = 0
    for name, a, sid, typ, kind, owner, pos in lst[:8]:
        t = struct.unpack('<I', read(a + 0x38, 4))[0]
        row = read(t + 0x20, 12)
        if not row:
            continue
        fwd = struct.unpack('<3f', row)
        if abs(fwd[0]) < 0.2 or abs(fwd[2]) < 0.2:   # (an easy one to tell apart: not along an axis)
            continue
        n += 1
        yaws = [('atan2(x,z)', math.atan2(fwd[0], fwd[2])), ('atan2(-x,-z)', math.atan2(-fwd[0], -fwd[2])),
                ('atan2(z,x)', math.atan2(fwd[2], fwd[0])), ('atan2(-z,x)', math.atan2(-fwd[2], fwd[0]))]
        found = set()
        walk(read, a, fwd, yaws, 0, '', found, {a})
        count.update(found)
    for k, c in count.most_common(40):
        if c >= 2:
            print('%-36s on %d of %d enemies' % (k, c, n))


if __name__ == '__main__':
    main()
