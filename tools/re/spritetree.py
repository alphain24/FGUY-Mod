"""Research: a movie's clip by its instance name (as placed on the root timeline, e.g. Icon_Scope_0_4) - its frames'
labels, and the named clips it places (and theirs, a few levels down), from the sprite definitions.

  spritetree.py <file.gfx> <instance name> [levels, default 3]
"""
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import swf  # noqa: E402
from placements import Bits, matrix  # noqa: E402


def place(data, body, code):
    """(character id or None, name or None) of a PlaceObject2 / 3."""
    flags = data[body]
    o = body + (2 if code == 70 else 1) + 2   # (flags[, flags2], depth)
    flags2 = data[body + 1] if code == 70 else 0
    if code == 70 and flags2 & 0x08:   # class name
        o = data.index(b'\0', o) + 1
    cid = None
    if flags & 0x02:
        cid = struct.unpack_from('<H', data, o)[0]
        o += 2
    if flags & 0x04:
        _, o = matrix(data, o)
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
    name = data[o:data.index(b'\0', o)].decode('latin-1') if flags & 0x20 else None
    return cid, name


def sprites(data):
    out = {}
    for code, body, ln, to in swf.tags(data, swf.header_end(data), len(data)):
        if code == 39:
            out[struct.unpack_from('<H', data, body)[0]] = (body + 4, body + ln)
    return out


def contents(data, start, end):
    labels, kids, frame = [], [], 0
    for code, body, ln, to in swf.tags(data, start, end):
        if code == 1:
            frame += 1
        elif code == 43:
            labels.append((frame + 1, data[body:data.index(b'\0', body)].decode('latin-1')))
        elif code in (26, 70):
            cid, name = place(data, body, code)
            if cid is not None:
                kids.append((frame + 1, cid, name))
    return labels, kids, frame


def show(data, sp, cid, name, depth, levels, seen):
    if cid not in sp:
        print('  ' * depth + '%s (#%d, not a clip)' % (name, cid))
        return
    labels, kids, frames = contents(data, *sp[cid])
    print('  ' * depth + '%s (#%d, %d frames)%s' % (name, cid, frames,
                                                  ' labels: ' + ', '.join('%d %s' % l for l in labels) if labels else ''))
    if depth >= levels or cid in seen:
        return
    seen = seen | {cid}
    done = set()
    for f, k, n in kids:
        if (k, n) in done:
            continue
        done.add((k, n))
        show(data, sp, k, '%s @frame %d' % (n or '-', f), depth + 1, levels, seen)


def main():
    data = open(sys.argv[1], 'rb').read()
    want = sys.argv[2]
    levels = int(sys.argv[3]) if len(sys.argv) > 3 else 3
    sp = sprites(data)
    for code, body, ln, to in swf.tags(data, swf.header_end(data), len(data)):
        if code in (26, 70):
            cid, name = place(data, body, code)
            if name == want and cid is not None:
                show(data, sp, cid, name, 0, levels, set())
                return
    print('not placed on the root:', want)


if __name__ == '__main__':
    main()
