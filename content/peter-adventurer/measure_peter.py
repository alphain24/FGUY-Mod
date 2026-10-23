"""Measures Peter's bind mesh (build/research/peter_rig.glb, the game's model exported locally - never shipped)
so the outfit pieces can be fitted: per-region extents (by the colour of his own texture under each vertex),
torso / leg / arm cross-sections, head. Writes measure.json (numbers only)."""
import json, struct, zlib, sys, math
from collections import defaultdict

GLB = sys.argv[1] if len(sys.argv) > 1 else '../../build/research/peter_rig.glb'
b = open(GLB, 'rb').read()
n = struct.unpack_from('<I', b, 12)[0]
j = json.loads(b[20:20 + n])
bin0 = 20 + n + 8


def view(i):
    v = j['bufferViews'][i]
    return b[bin0 + v.get('byteOffset', 0): bin0 + v.get('byteOffset', 0) + v['byteLength']]


def acc(i):
    a = j['accessors'][i]
    comps = {'SCALAR': 1, 'VEC2': 2, 'VEC3': 3, 'VEC4': 4, 'MAT4': 16}[a['type']]
    fmt = {5126: 'f', 5123: 'H', 5121: 'B', 5125: 'I'}[a['componentType']]
    size = struct.calcsize(fmt)
    data = view(a['bufferView'])
    off = a.get('byteOffset', 0)
    stride = j['bufferViews'][a['bufferView']].get('byteStride', comps * size)
    return [struct.unpack_from('<' + fmt * comps, data, off + k * stride) for k in range(a['count'])]


def png(data):
    assert data[:8] == b'\x89PNG\r\n\x1a\n'
    p, idat, w = 8, b'', 0
    while p < len(data):
        ln, = struct.unpack_from('>I', data, p)
        t = data[p + 4:p + 8]
        c = data[p + 8:p + 8 + ln]
        if t == b'IHDR':
            w, h, depth, ct = struct.unpack_from('>IIBB', c)
            assert depth == 8 and ct in (2, 6), (depth, ct)
            bpp = 3 if ct == 2 else 4
        elif t == b'IDAT':
            idat += c
        p += 12 + ln
    raw = zlib.decompress(idat)
    stride = w * bpp
    out = bytearray(h * stride)
    prev = bytearray(stride)
    q = 0
    for y in range(h):
        f = raw[q]; q += 1
        line = bytearray(raw[q:q + stride]); q += stride
        for x in range(stride):
            a = line[x - bpp] if x >= bpp else 0
            up = prev[x]
            c = prev[x - bpp] if x >= bpp else 0
            if f == 1: line[x] = (line[x] + a) & 255
            elif f == 2: line[x] = (line[x] + up) & 255
            elif f == 3: line[x] = (line[x] + ((a + up) >> 1)) & 255
            elif f == 4:
                pa, pb, pc = abs(up - c), abs(a - c), abs(a + up - 2 * c)
                pr = a if pa <= pb and pa <= pc else (up if pb <= pc else c)
                line[x] = (line[x] + pr) & 255
        out[y * stride:(y + 1) * stride] = line
        prev = line
    return w, h, bpp, out


tex = {}
for mi, mesh in enumerate(j['meshes']):
    pass
img = j['images'][j['textures'][1]['source']] if len(j['textures']) > 1 else None
W, H, BPP, PIX = png(view(j['images'][1]['bufferView']))


def colour(u, v):
    x = min(W - 1, max(0, int((u % 1.0) * W)))
    y = min(H - 1, max(0, int((v % 1.0) * H)))
    k = (y * W + x) * BPP
    return PIX[k], PIX[k + 1], PIX[k + 2]


def classify(c):
    r, g, bl = c
    if r > 200 and g > 200 and bl > 200: return 'shirt'
    if g > r + 15 and g > bl: return 'trousers'
    if r > 200 and 150 < g < 225 and bl < 200 and r > bl + 30: return 'skin'
    if r < 70 and g < 70 and bl < 70: return 'black'
    if r > 180 and g > 90 and bl < 90: return 'orange'
    if r > g > bl and r < 170: return 'brown'
    return 'other'


regions = defaultdict(list)
allv = []
for mi, mesh in enumerate(j['meshes']):
    for prim in mesh['primitives']:
        if j['materials'][prim['material']]['pbrMetallicRoughness']['baseColorTexture']['index'] != 1:
            continue
        P = acc(prim['attributes']['POSITION'])
        UV = acc(prim['attributes']['TEXCOORD_0'])
        for p, uv in zip(P, UV):
            k = classify(colour(*uv))
            regions[k].append(p)
            allv.append(p)


def box(ps):
    if not ps: return None
    return [[round(min(p[i] for p in ps), 4) for i in range(3)], [round(max(p[i] for p in ps), 4) for i in range(3)]]


out = {'bbox': box(allv), 'regions': {k: {'count': len(v), 'box': box(v)} for k, v in regions.items()}}


def sections(ps, axis_lo, axis_hi, steps, side=None):
    """Horizontal cross-sections (y) of a point set: x min/max, z min/max per band."""
    res = []
    for s in range(steps + 1):
        y = axis_lo + (axis_hi - axis_lo) * s / steps
        band = [p for p in ps if abs(p[1] - y) < (axis_hi - axis_lo) / steps * 0.6 and (side is None or side(p))]
        if band:
            res.append({'y': round(y, 4), 'x': [round(min(p[0] for p in band), 4), round(max(p[0] for p in band), 4)],
                        'z': [round(min(p[2] for p in band), 4), round(max(p[2] for p in band), 4)], 'n': len(band)})
    return res


bb = out['bbox']
yl, yh = bb[0][1], bb[1][1]
body = regions['shirt'] + regions['trousers'] + regions['black'] + regions['orange']
torso = [p for p in regions['shirt'] if abs(p[0]) < (bb[1][0] - bb[0][0]) * 0.22]
out['torsoSections'] = sections(torso, min(p[1] for p in torso), max(p[1] for p in torso), 12)
tr = regions['trousers']
out['legSections'] = {
    'left(+x)': sections(tr, min(p[1] for p in tr), max(p[1] for p in tr), 8, lambda p: p[0] > 0),
    'right(-x)': sections(tr, min(p[1] for p in tr), max(p[1] for p in tr), 8, lambda p: p[0] < 0)}
head = [p for p in regions['skin'] + regions['brown'] + regions['other'] if p[1] > yl + (yh - yl) * 0.62 and abs(p[0]) < (bb[1][0] - bb[0][0]) * 0.2]
out['headBox'] = box(head)
out['headSections'] = sections(head, min(p[1] for p in head), max(p[1] for p in head), 10)
sleeves = [p for p in regions['shirt'] if abs(p[0]) >= (bb[1][0] - bb[0][0]) * 0.22]
out['sleeveBox'] = {'left(+x)': box([p for p in sleeves if p[0] > 0]), 'right(-x)': box([p for p in sleeves if p[0] < 0])}
feet = [p for p in allv if p[1] < yl + (yh - yl) * 0.12]
out['feet'] = {'left(+x)': box([p for p in feet if p[0] > 0]), 'right(-x)': box([p for p in feet if p[0] < 0])}
out['belt'] = box(regions['black'] and [p for p in regions['black'] if yl + (yh - yl) * 0.3 < p[1] < yl + (yh - yl) * 0.6])
out['buckle'] = box(regions['orange'])
# arm (skin) beyond the sleeves
arms = [p for p in regions['skin'] if abs(p[0]) >= (bb[1][0] - bb[0][0]) * 0.22]
out['armSkinBox'] = {'left(+x)': box([p for p in arms if p[0] > 0]), 'right(-x)': box([p for p in arms if p[0] < 0])}
# bones (inverse binds -> bind positions)
skin = j['skins'][0]
ib = acc(skin['inverseBindMatrices'])
bones = []
for k, node in enumerate(skin['joints']):
    m = ib[k]  # column-major inverse bind; bind position = -R^T t
    R = [[m[0], m[4], m[8]], [m[1], m[5], m[9]], [m[2], m[6], m[10]]]
    t = [m[12], m[13], m[14]]
    pos = [-(R[0][i] * t[0] + R[1][i] * t[1] + R[2][i] * t[2]) for i in range(3)]
    bones.append({'i': k, 'name': j['nodes'][node].get('name'), 'pos': [round(x, 4) for x in pos]})
out['bones'] = bones
json.dump(out, open('measure.json', 'w'), indent=1)
print(json.dumps({k: v for k, v in out.items() if k not in ('bones', 'torsoSections', 'legSections', 'headSections')}, indent=1))
