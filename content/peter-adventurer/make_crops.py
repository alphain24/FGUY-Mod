"""Cuts one reference crop per material from ref_big.png (2x reference.png) into material-evidence/,
plus a contact sheet (crops.png) to check each crop sits on the part it names."""
import os, struct, zlib, sys

sys.path.insert(0, os.path.expanduser('~/.claude/skills/img2threejs/forge/stage1_intake'))
from extract_pbr_evidence import load_image  # noqa: E402

CROPS = {  # material id: (x0, y0, x1, y1) in ref_big.png pixels
    'hat-felt': (215, 35, 320, 85), 'hat-band': (215, 112, 330, 126), 'vest-leather': (150, 600, 230, 655),
    'vest-flap': (185, 535, 255, 562), 'brass': (322, 714, 358, 721), 'boot-leather': (110, 905, 190, 960),
    'lace': (418, 872, 430, 884), 'sheath-leather': (85, 770, 130, 830), 'pendant': (326, 388, 346, 410),
    'cuff': (45, 480, 95, 512),
}


def write_png(path, w, h, rows):
    raw = b''.join(b'\x00' + bytes(r) for r in rows)
    def chunk(t, d):
        return struct.pack('>I', len(d)) + t + d + struct.pack('>I', zlib.crc32(t + d) & 0xffffffff)
    open(path, 'wb').write(b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 6, 0, 0, 0))
                           + chunk(b'IDAT', zlib.compress(raw, 9)) + chunk(b'IEND', b''))


from pathlib import Path
W, H, px, _ = load_image(Path('ref_big.png'))  # flat list of (r, g, b, a)


def get(x, y):
    return list(px[y * W + x])


os.makedirs('material-evidence', exist_ok=True)
sheet = []
for mid, (x0, y0, x1, y1) in CROPS.items():
    rows = [[c for x in range(x0, x1) for c in get(x, y)] for y in range(y0, y1)]
    write_png(f'material-evidence/{mid}.png', x1 - x0, y1 - y0, rows)
    sheet.append((mid, rows, x1 - x0, y1 - y0))
# contact sheet: each crop scaled x3, stacked
S = 3
cw = max(w for _, _, w, _ in sheet) * S
out = []
for mid, rows, w, h in sheet:
    for r in rows:
        line = []
        for x in range(w):
            line += r[x * 4:x * 4 + 4] * S
        line += [255, 255, 255, 255] * (cw - w * S)
        for _ in range(S):
            out.append(line)
    out += [[255, 255, 255, 255] * cw] * 6
write_png('material-evidence/crops.png', cw, len(out), out)
print(W, H, 'ok', list(CROPS))
