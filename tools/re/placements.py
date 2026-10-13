"""Where a movie places its named clips: stage size, and each PlaceObject2 whose name matches (position, scale,
character, and that character's shape bounds). Usage: placements.py <file.gfx> <name substring>"""
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import swf  # noqa: E402


class Bits:
    def __init__(self, data, o):
        self.d, self.o, self.b = data, o, 0

    def ub(self, n):
        v = 0
        for _ in range(n):
            byte = self.d[self.o]
            v = (v << 1) | ((byte >> (7 - self.b)) & 1)
            self.b += 1
            if self.b == 8:
                self.b, self.o = 0, self.o + 1
        return v

    def sb(self, n):
        v = self.ub(n)
        return v - (1 << n) if n and v & (1 << (n - 1)) else v

    def align(self):
        if self.b:
            self.b, self.o = 0, self.o + 1


def rect(data, o):
    b = Bits(data, o)
    n = b.ub(5)
    v = [b.sb(n) / 20 for _ in range(4)]
    b.align()
    return v, b.o   # xmin, xmax, ymin, ymax (pixels)


def matrix(data, o):
    b = Bits(data, o)
    sx = sy = 1.0
    if b.ub(1):
        n = b.ub(5)
        sx, sy = b.sb(n) / 65536, b.sb(n) / 65536
    if b.ub(1):
        n = b.ub(5)
        b.sb(n), b.sb(n)
    n = b.ub(5)
    tx, ty = b.sb(n) / 20, b.sb(n) / 20
    b.align()
    return (sx, sy, tx, ty), b.o


def main():
    data = open(sys.argv[1], 'rb').read()
    want = sys.argv[2]
    stage, _ = rect(data, 8)
    print(f'stage {stage[1] - stage[0]:.0f} x {stage[3] - stage[2]:.0f}')
    all_tags = swf.walk(data, swf.header_end(data), len(data))
    bounds = {}
    for depth, code, body, ln, to in all_tags:
        if code in (2, 22, 32, 83):   # DefineShape*: id, bounds
            cid = struct.unpack_from('<H', data, body)[0]
            bounds[cid] = rect(data, body + 2)[0]
    sprites = {}
    for depth, code, body, ln, to in all_tags:
        if code == 39:
            sprites[struct.unpack_from('<H', data, body)[0]] = (body, ln)
    for depth, code, body, ln, to in all_tags:
        if code != 26:
            continue
        flags = data[body]
        o = body + 3   # flags, depth(2)
        cid = None
        if flags & 0x02:
            cid = struct.unpack_from('<H', data, o)[0]
            o += 2
        mtx = None
        if flags & 0x04:
            mtx, o = matrix(data, o)
        if flags & 0x08:   # color transform: skip (CXFORMWITHALPHA)
            b = Bits(data, o)
            has_add, has_mult = b.ub(1), b.ub(1)
            n = b.ub(4)
            for _ in range((4 if has_mult else 0) + (4 if has_add else 0)):
                b.sb(n)
            b.align()
            o = b.o
        if flags & 0x10:
            o += 2   # ratio
        name = None
        if flags & 0x20:
            end = data.index(b'\0', o)
            name = data[o:end].decode('latin-1')
        if name and want in name:
            inner = []
            if cid in sprites:
                sb, sl = sprites[cid]
                for c2, b2, l2, t2 in swf.tags(data, sb + 4, sb + sl):
                    if c2 == 26 and data[b2] & 0x02:
                        inner.append(struct.unpack_from('<H', data, b2 + 3)[0])
            shapes = [(i, bounds.get(i)) for i in inner if i in bounds]
            print(f'{name}: depth-level {depth}, character {cid}, matrix {mtx}, shapes inside {shapes[:4]}')


if __name__ == '__main__':
    main()
