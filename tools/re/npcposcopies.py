"""Research: where else an enemy keeps its position - float triples equal (within 0.05) to its transform's position, in
the enemy object and in the objects it points to (two steps), for a few enemies: the same places for each = its body.

  npcposcopies.py <pid>
"""
import collections
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from findobj import open_reader  # noqa: E402
from npcs import npcs  # noqa: E402
from pe import PE  # noqa: E402


def near(b, o, p):
    if o + 12 > len(b):
        return False
    v = struct.unpack_from('<3f', b, o)
    return all(abs(v[i] - p[i]) < 0.05 for i in range(3))


def walk(read, a, p, depth, path, out, seen):
    b = read(a, 0x1200 if depth == 0 else 0x400)
    if not b:
        return
    for o in range(0, len(b) - 11, 4):
        if near(b, o, p):
            out.add('%s+%x' % (path, o))
    if depth >= 2:
        return
    for o in range(0, len(b) - 3, 4):
        q = struct.unpack_from('<I', b, o)[0]
        if 0x01000000 < q < 0x7fff0000 and q not in seen:
            seen.add(q)
            walk(read, q, p, depth + 1, '%s[+%x]' % (path, o), out, seen)


def main():
    pid = int(sys.argv[1])
    h, read = open_reader(pid)
    lst = npcs(pid, PE(os.environ.get('FGUY_EXE', 'A:/Games/FGUY-Research/FGUY.exe')))
    count = collections.Counter()
    for name, a, sid, typ, kind, owner, pos in lst[:4]:
        found = set()
        walk(read, a, pos, 0, '', found, {a})
        count.update(found)
    for k, n in count.most_common(30):
        if n >= 2:
            print('%-28s on %d of %d enemies' % (k, n, min(4, len(lst))))


if __name__ == '__main__':
    main()
