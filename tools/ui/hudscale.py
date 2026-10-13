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


# How each element goes from my 4-way quarter to its single-player place (measured in the game: an offline
# one-player match's layout next to an online one's, at 16:9). Most have the same inner layout in both, so their
# single-player anchor and scale is it: the movie's single-player placement, plus the HUD's own widescreen shift
# (ShiftWideScreenElements: 128 to the left / right for the elements in its WideScreenLeftList / RightList -
# done in pause.as2js, it knows the screen). These differ inside:
WHOLE_SCREEN = {'Icon_Scope', 'Icon_Curtain', 'Icon_Screen', 'Icon_TextBox1', 'Icon_TextBox6'}   # zoom alone
DAMAGE_FRAME = 'Icon_DamageFx'                      # fitted to the screen by FGUYFitDamageFx
OUTLINE = {'Icon_MultiplayerStats': (49, -555)}     # placed by what it draws: its top left from its anchor


def layout_table(data):
    """{name: [scale x, scale y, single-player x, y, kind, outline dx, dy]} (kind 0 anchor, 1 outline, 2 zoom
    alone, 3 the damage frame): online my 4-way elements go where, and at the size, the single-player HUD has
    them (src/ui/pause.as2js FGUYHudZoom)."""
    p = root_placements(data)
    out = {}
    for name, (rx, ry) in scale_ratios(data).items():
        m0 = p[name + '_0_0']
        kind = 3 if name == DAMAGE_FRAME else 2 if name in WHOLE_SCREEN else 1 if name in OUTLINE else 0
        dx, dy = OUTLINE.get(name, (0, 0))
        out[name] = [rx, ry, round(m0[2], 2), round(m0[3], 2), kind, dx, dy]
    return out


if __name__ == '__main__':
    r = scale_ratios(open(sys.argv[1], 'rb').read())
    for k in sorted(r):
        print(k, r[k])
