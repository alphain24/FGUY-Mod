"""Research: the NPCs in a running game - every object whose class (RTTI) is an NPC entity, with where it is, its
entity type (+0x20), its id (+0x18), the +0x10 kind word (0x11f: summoned) and the owner of a summoned one (+0x8e4).

  npcs.py <pid> [<pid> ...]          (several: each PC's list, to compare)
"""
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from findobj import open_reader  # noqa: E402
from pe import PE  # noqa: E402
from vtcount import count  # noqa: E402
from vtname import name_of  # noqa: E402

EXE = os.environ.get('FGUY_EXE', 'A:/Games/FGUY-Research/FGUY.exe')
KEEP = ('NPCEntity', 'NPCGeneric', 'NPCBase')


def npcs(pid, exe):
    h, read = open_reader(pid)
    out = []
    for vt, objs in count(h, read).items():
        name = name_of(exe, vt)
        if not any(k in name for k in KEEP) or '(at +' in name:
            continue
        for a in objs:
            b = read(a, 0x40)
            if not b:
                continue
            m = struct.unpack_from('<I', b, 0x38)[0]
            p = read(m + 0x30, 12) if 0x10000 < m < 0x7fff0000 else None
            if not p:
                continue
            x, y, z = struct.unpack('<3f', p)
            if not all(abs(v) < 1e5 for v in (x, y, z)):
                continue
            kind = struct.unpack_from('<I', b, 0x10)[0]
            owner = 0
            if kind == 0x11f:
                o = read(a + 0x8e4, 4)
                owner = struct.unpack('<I', o)[0] if o else 0
            out.append((name.replace('.?AV', '').replace('@@', ''), a, struct.unpack_from('<I', b, 0x18)[0],
                        struct.unpack_from('<I', b, 0x20)[0], kind, owner, (x, y, z)))
    return sorted(out, key=lambda e: (e[0], e[6]))


def main():
    exe = PE(EXE)
    for pid in map(int, sys.argv[1:]):
        lst = npcs(pid, exe)
        print('== %d: %d NPCs' % (pid, len(lst)))
        for name, a, sid, typ, kind, owner, (x, y, z) in lst:
            print('  %-14s %08x id %6x type %3x kind %3x%s at (%6.1f %5.1f %6.1f)' % (
                name, a, sid, typ, kind, ' owner %08x' % owner if owner else '', x, y, z))


if __name__ == '__main__':
    main()
