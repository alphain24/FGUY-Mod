# Two PCs' camera dumps (FGUYMod\cam_<pid>.bin, FGUYMOD_CAMDUMP=1): the first tick where they differ, and where.
#   python tools/cam_compare.py <cam_A.bin> <cam_B.bin> [from_tick]
import struct
import sys

BLOCK = 0x400
PARTS = [('viewport 0 controller', 0), ('viewport 0 camera+0x100', 0x100), ('viewport 1 controller', 0),
         ('viewport 1 camera+0x100', 0x100)]
REC = 4 + BLOCK * len(PARTS)


def load(path):
    d = open(path, 'rb').read()
    out = {}
    for i in range(0, len(d) - REC + 1, REC):
        out[struct.unpack_from('<I', d, i)[0]] = d[i + 4:i + REC]
    return out


def pointer(x, y, o):   # (addresses differ between processes anyway: as floats they're tiny)
    fx, fy = struct.unpack_from('<f', x, o)[0], struct.unpack_from('<f', y, o)[0]
    return 0 < abs(fx) < 1e-6 and 0 < abs(fy) < 1e-6 or abs(fx) > 1e30 or abs(fy) > 1e30


a, b = load(sys.argv[1]), load(sys.argv[2])
start = int(sys.argv[3]) if len(sys.argv) > 3 else 0
shown = 0
for t in sorted(set(a) & set(b)):
    if t < start or a[t] == b[t]:
        continue
    lines = []
    for k, (name, base) in enumerate(PARTS):
        x, y = a[t][k * BLOCK:(k + 1) * BLOCK], b[t][k * BLOCK:(k + 1) * BLOCK]
        offs = [o for o in range(0, BLOCK, 4) if x[o:o + 4] != y[o:o + 4] and not pointer(x, y, o)]
        if not offs:
            continue
        desc = []
        for o in offs[:16]:
            fx, fy = struct.unpack_from('<f', x, o)[0], struct.unpack_from('<f', y, o)[0]
            ix, iy = struct.unpack_from('<I', x, o)[0], struct.unpack_from('<I', y, o)[0]
            desc.append(f'+{base + o:#x}: {fx:.6g}/{fy:.6g} ({ix:#x}/{iy:#x})')
        lines.append(f'  {name}: {len(offs)} words differ: ' + '; '.join(desc))
    if not lines:
        continue
    print(f'tick {t}:')
    print('\n'.join(lines))
    shown += 1
    if shown >= 4:
        break
if not shown:
    print('the same every tick')
