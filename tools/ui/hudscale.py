"""For each HUD element (Icon_<name>), its single-player (layout 0) scale over its 4-way (layout 4) scale, from
the movie's own placements: online the 4-way HUD is shown zoomed into one quarter, and its elements are scaled
by this so they come out at their single-player size (src/ui/pause.as2js FGUYHudZoom)."""
import os
import re
import struct
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 're'))
import swf  # noqa: E402
from placements import Bits, matrix  # noqa: E402


def root_placements(data):
    """{name: (sx, sy, tx, ty)} of the named clips the root timeline places."""
    out = {}
    for code, body, ln, to in swf.tags(data, swf.header_end(data), len(data)):
        if code != 26:
            continue
        flags = data[body]
        o = body + 3
        if flags & 0x02:
            o += 2
        m = (1.0, 1.0, 0.0, 0.0)
        if flags & 0x04:
            m, o = matrix(data, o)
        if flags & 0x08:
            b = Bits(data, o)
            has_add, has_mult = b.ub(1), b.ub(1)
            n = b.ub(4)
            for _ in range((4 if has_mult else 0) + (4 if has_add else 0)):
                b.sb(n)
            b.align()
            o = b.o
        if flags & 0x10:
            o += 2
        if flags & 0x20:
            out[data[o:data.index(b'\0', o)].decode('latin-1')] = m
    return out


def scale_ratios(data):
    p = root_placements(data)
    out = {}
    for name, m4 in p.items():
        g = re.match(r'^(.+)_0_4$', name)
        if not g or (g.group(1) + '_0_0') not in p:
            continue
        m0 = p[g.group(1) + '_0_0']
        if m4[0] and m4[1]:
            out[g.group(1)] = (round(abs(m0[0] / m4[0]), 4), round(abs(m0[1] / m4[1]), 4))
    return out


if __name__ == '__main__':
    r = scale_ratios(open(sys.argv[1], 'rb').read())
    for k in sorted(r):
        print(k, r[k])
