"""Research: what changes in a running game's player objects? Reads FGUY.exe's memory (read only).

  statewatch.py <pid> snap <file>                  save every player's whole object (and what it points to)
  statewatch.py <pid> diff <file> [<player>]       list the 4-byte values that changed since <file>
  statewatch.py <pid> find <float> [<player>]      where a player's object holds a float near this value
  statewatch.py <pid> list                         the players, their positions and vtables
The player list is the game's own (0xbad988: pointers, count at +0x20). An object is 0x11000 bytes
(zFamilyGuyPlayer reaches at least +0x10b3c); each pointer in it is followed one level (0x400 bytes).
"""
import os
import pickle
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from memwatch import players, reader  # noqa: E402

OBJ = 0x11000
PTR = 0x400


def snapshot(read, only=None):
    blocks = {}
    for pi, p in enumerate(players(read)):
        if only is not None and pi != only:
            continue
        obj = read(p, OBJ)
        if not obj:
            continue
        blocks[(pi, -1)] = obj
        for off in range(0, len(obj), 4):
            v = struct.unpack_from('<I', obj, off)[0]
            if 0x01000000 <= v < 0x7fff0000 and v % 4 == 0 and not (p <= v < p + OBJ):
                b = read(v, PTR)
                if b:
                    blocks[(pi, off)] = b
    return blocks


def show(v):
    f = struct.unpack('<f', struct.pack('<I', v))[0]
    fs = f'{f:.4g}' if f == f and 1e-6 < abs(f) < 1e7 else '-'
    return f'{v:08x} ({v if v < 0x80000000 else v - 0x100000000:d} / {fs})'


def main():
    pid, cmd = int(sys.argv[1]), sys.argv[2]
    read = reader(pid)
    if cmd == 'list':
        for pi, p in enumerate(players(read)):
            vt = struct.unpack('<I', read(p, 4))[0]
            t = struct.unpack('<I', read(p + 0x38, 4))[0]
            pos = struct.unpack('<3f', read(t + 0x30, 12)) if t else (0, 0, 0)
            print(f'p{pi} {p:#x} vtable {vt:#x} pos ({pos[0]:.2f}, {pos[1]:.2f}, {pos[2]:.2f})')
        return
    if cmd == 'snap':
        snap = snapshot(read)
        pickle.dump(snap, open(sys.argv[3], 'wb'))
        print(f'{len(snap)} blocks saved')
        return
    if cmd == 'find':
        want = float(sys.argv[3])
        only = int(sys.argv[4]) if len(sys.argv) > 4 else None
        for (pi, ptr), b in sorted(snapshot(read, only).items()):
            for off in range(0, len(b) - 3, 4):
                f = struct.unpack_from('<f', b, off)[0]
                if f == f and abs(f - want) <= max(1e-3, abs(want) * 1e-4):
                    where = f'p{pi} +{off:#x}' if ptr < 0 else f'p{pi} [+{ptr:#x}]+{off:#x}'
                    print(f'{where}: {f}')
        return
    only = int(sys.argv[4]) if len(sys.argv) > 4 else None
    old = pickle.load(open(sys.argv[3], 'rb'))
    new = snapshot(read, only)
    for key in sorted(new):
        if key not in old:
            continue
        a, b = old[key], new[key]
        pi, ptr = key
        for off in range(0, min(len(a), len(b)) - 3, 4):
            x, y = struct.unpack_from('<I', a, off)[0], struct.unpack_from('<I', b, off)[0]
            if x != y:
                where = f'p{pi} +{off:#x}' if ptr < 0 else f'p{pi} [+{ptr:#x}]+{off:#x}'
                print(f'{where}: {show(x)} -> {show(y)}')


if __name__ == '__main__':
    main()
