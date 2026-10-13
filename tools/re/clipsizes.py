"""The size of every named clip a movie places on its root (union of its contents' shape bounds, through nested
sprites, translations and scales only). Usage: clipsizes.py <file.gfx> [min width]"""
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import swf  # noqa: E402
from placements import matrix, rect  # noqa: E402  (placements.py runs as a script: guard below)

data = open(sys.argv[1], 'rb').read()
min_w = float(sys.argv[2]) if len(sys.argv) > 2 else 600
top = swf.header_end(data)
shape_bounds, sprites = {}, {}
for depth, code, body, ln, to in swf.walk(data, top, len(data)):
    if code in (2, 22, 32, 83):
        shape_bounds[struct.unpack_from('<H', data, body)[0]] = rect(data, body + 2)[0]
    elif code == 39:
        sprites[struct.unpack_from('<H', data, body)[0]] = (body + 4, body + ln)


def placements_in(start, end):
    out = []
    for code, body, ln, to in swf.tags(data, start, end):
        if code != 26:
            continue
        flags = data[body]
        o = body + 3
        cid = None
        if flags & 0x02:
            cid = struct.unpack_from('<H', data, o)[0]
            o += 2
        mtx = (1.0, 1.0, 0.0, 0.0)
        if flags & 0x04:
            mtx, o = matrix(data, o)
        name = None
        if flags & 0x20 and not flags & 0x08 and not flags & 0x10:
            end_ = data.index(b'\0', o)
            name = data[o:end_].decode('latin-1')
        if cid is not None:
            out.append((cid, mtx, name))
    return out


cache = {}


def bounds(cid, depth=0):
    if cid in cache:
        return cache[cid]
    if cid in shape_bounds:
        return shape_bounds[cid]
    if cid not in sprites or depth > 20:
        return None
    b = None
    for child, (sx, sy, tx, ty), _ in placements_in(*sprites[cid]):
        cb = bounds(child, depth + 1)
        if not cb:
            continue
        xs = [cb[0] * sx + tx, cb[1] * sx + tx]
        ys = [cb[2] * sy + ty, cb[3] * sy + ty]
        nb = [min(xs), max(xs), min(ys), max(ys)]
        b = nb if b is None else [min(b[0], nb[0]), max(b[1], nb[1]), min(b[2], nb[2]), max(b[3], nb[3])]
    cache[cid] = b
    return b


for cid, (sx, sy, tx, ty), name in placements_in(top, len(data)):
    b = bounds(cid)
    if not b:
        continue
    w, h = (b[1] - b[0]) * abs(sx), (b[3] - b[2]) * abs(sy)
    if w >= min_w:
        x0 = tx + min(b[0] * sx, b[1] * sx)
        y0 = ty + min(b[2] * sy, b[3] * sy)
        print(f'{name or "(unnamed)"}: character {cid}, {w:.0f} x {h:.0f} at ({x0:.0f}, {y0:.0f})')
