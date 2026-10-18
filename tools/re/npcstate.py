"""Research: each PC's enemies side by side by id - where each is and its health ([+0x420] +0x60 / +0x64: health / max,
as the game's damage 0x99eec0 reads it for an enemy).

  npcstate.py <host pid> <other pid>
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
    out = {}
    for name, a, sid, typ, kind, owner, pos in npcs(pid, exe):
        c = struct.unpack('<I', r(a + 0x420, 4))[0]
        hp = struct.unpack('<2f', r(c + 0x60, 8)) if 0x10000 < c < 0x7fff0000 else (float('nan'), float('nan'))
        out[sid] = (pos, hp)
    return out


def main():
    exe = PE(os.environ.get('FGUY_EXE', 'A:/Games/FGUY-Research/FGUY.exe'))
    a, b = (state(int(p), exe) for p in sys.argv[1:3])
    for sid in sorted(set(a) | set(b)):
        pa, ha = a.get(sid, (None, None))
        pb, hb = b.get(sid, (None, None))
        d = math.dist(pa, pb) if pa and pb else float('nan')
        fmt = lambda p, hp: '(%6.1f %5.1f %6.1f) %5.0f/%-4.0f' % (*p, *hp) if p else '%-30s' % '-'
        print('id %x  host %s  other %s  %5.1f apart' % (sid, fmt(pa, ha), fmt(pb, hb), d))


if __name__ == '__main__':
    main()
