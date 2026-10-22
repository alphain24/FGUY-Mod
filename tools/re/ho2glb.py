"""Research: one of the game's character models (a Model asset in a .ho) to a .glb - its mesh pieces (SkinGeometry) with
their texture coordinates and color textures, unposed (bind pose), no skeleton yet. For checking the format; the output
is the game's art - kept on this PC, never shared.

  ho2glb.py <file.ho> <model name or id lo hex> <out.glb> [--info]"""
import io
import json
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import ho  # noqa: E402

T_MODEL, T_SKIN, T_MAT, T_TEX, T_RAW = 0x5247BB31, 0x66EC031B, 0x189F55BF, 0x2952081F, 0xBDE21A8D


def refs(d, by, ids):
    out = []
    for i in range(0, len(d) - 7, 4):
        k = struct.unpack_from('<II', d, i)
        if k in ids:
            out.append((i, by[k]))
    return out


def half(h):
    return struct.unpack('<e', struct.pack('<H', h))[0]


def piece(g, by, ids):
    d = g.data()
    n = struct.unpack_from('<H', d, 0x54)[0]
    raws = [a for _, a in refs(d, by, ids) if a.type == T_RAW]
    mats = [a for _, a in refs(d, by, ids) if a.type == T_MAT]
    uv = next((a for a in raws if a.size == n * 4), None)
    rest = sorted((a for a in raws if a is not uv), key=lambda a: -a.size)
    vb = rest[0] if rest else None   # (the biggest: the vertices; the other: the triangles)
    ib = rest[1] if len(rest) > 1 else None
    return n, vb, uv, ib, mats


def vertex_starts(vd, n):
    """Where each vertex starts: 28 bytes each (position, normal, one bone) - or, skinned to more bones, 32 / 36 (their
    weights): walked by the normals (unit length) - research; the influence count's encoding isn't decoded yet."""
    import math

    def unit(o):
        if o + 24 > len(vd):
            return True
        x, y, z = struct.unpack_from('<3f', vd, o + 12)
        return abs(math.sqrt(x * x + y * y + z * z) - 1) < 0.02
    if len(vd) % n == 0:
        return [i * (len(vd) // n) for i in range(n)]
    out, pos = [], 0
    while len(out) < n and pos + 24 <= len(vd):
        out.append(pos)
        pos += next((s for s in (28, 32, 36, 40) if unit(pos + s)), 28)
    return out


def texture_of(mat, by, ids):
    """The material's first color texture (a Texture asset: 16 bytes naming its DDS blob)."""
    for _, a in refs(mat.data(), by, ids):
        if a.type == T_TEX:
            lo, hi = struct.unpack_from('<II', a.data(), 0)
            blob = by.get((lo, hi))
            if blob:
                return a.name, blob.data()
    return None, None


def main():
    h = ho.Ho(sys.argv[1])
    by = {(a.lo, a.hi): a for a in h.assets}
    ids = set(by)
    want = sys.argv[2]
    model = next(a for a in h.assets if a.type == T_MODEL and (a.name == want or '%x' % a.lo == want.lower()))
    parts = [a for _, a in refs(model.data(), by, ids) if a.type == T_SKIN]
    if '--parts' in sys.argv:   # (research: only these pieces, e.g. 0-7 or 8)
        lo_hi = sys.argv[sys.argv.index('--parts') + 1].split('-')
        parts = parts[int(lo_hi[0]):int(lo_hi[-1]) + 1]
    info = '--info' in sys.argv
    gl = {'asset': {'version': '2.0', 'generator': 'FGUY Mod research'}, 'scene': 0, 'scenes': [{'nodes': []}],
          'nodes': [], 'meshes': [], 'accessors': [], 'bufferViews': [], 'buffers': [], 'materials': [],
          'textures': [], 'images': [], 'samplers': [{}]}
    blob = bytearray()

    def view(b, target=None):
        while len(blob) % 4:
            blob.append(0)
        gl['bufferViews'].append({'buffer': 0, 'byteOffset': len(blob), 'byteLength': len(b), **({'target': target} if target else {})})
        blob.extend(b)
        return len(gl['bufferViews']) - 1

    tex_index = {}
    for g in parts:
        n, vb, uv, ib, mats = piece(g, by, ids)
        tname, dds = texture_of(mats[0], by, ids) if mats else (None, None)
        print(f'{g.name or hex(g.lo)}: {n} verts, vb {vb.size if vb else None} ({vb.size // n if vb and n else "?"} each), '
              f'uv {uv.size if uv else None}, ib {ib.size if ib else None}, material {mats[0].name if mats else None}, texture {tname}')
        if info or not (vb and ib):
            continue
        vd, pos, nor, tc = vb.data(), [], [], []
        starts = vertex_starts(vd, n)
        if len(starts) != n:
            print('   (vertices not walked through - skipped)')
            continue
        for o in starts:
            pos += struct.unpack_from('<3f', vd, o)
            nor += struct.unpack_from('<3f', vd, o + 12)
        if uv:
            ud = uv.data()
            for v in range(n):
                u, w = struct.unpack_from('<HH', ud, v * 4)
                tc += [half(u), half(w)]
        idx = list(struct.unpack('<%dH' % (ib.size // 2), ib.data()))
        idx = [i for i in idx[:len(idx) // 3 * 3]]
        if max(idx) >= n:
            print('   (indices past the vertices - skipped)')
            continue
        mins = [min(pos[i::3]) for i in range(3)]
        maxs = [max(pos[i::3]) for i in range(3)]
        acc = lambda bv, comp, cnt, typ, **kw: gl['accessors'].append({'bufferView': bv, 'componentType': comp, 'count': cnt, 'type': typ, **kw}) or len(gl['accessors']) - 1
        a_pos = acc(view(struct.pack('<%df' % len(pos), *pos), 34962), 5126, n, 'VEC3', min=mins, max=maxs)
        a_nor = acc(view(struct.pack('<%df' % len(nor), *nor), 34962), 5126, n, 'VEC3')
        attrs = {'POSITION': a_pos, 'NORMAL': a_nor}
        if tc:
            attrs['TEXCOORD_0'] = acc(view(struct.pack('<%df' % len(tc), *tc), 34962), 5126, n, 'VEC2')
        a_idx = acc(view(struct.pack('<%dH' % len(idx), *idx), 34963), 5123, len(idx), 'SCALAR')
        mat = {'name': mats[0].name if mats else 'none', 'pbrMetallicRoughness': {'metallicFactor': 0, 'roughnessFactor': 1}}
        if dds:
            if tname not in tex_index:
                from PIL import Image
                png = io.BytesIO()
                Image.open(io.BytesIO(dds)).save(png, 'PNG')
                gl['images'].append({'bufferView': view(png.getvalue()), 'mimeType': 'image/png', 'name': tname})
                gl['textures'].append({'source': len(gl['images']) - 1, 'sampler': 0})
                tex_index[tname] = len(gl['textures']) - 1
            mat['pbrMetallicRoughness']['baseColorTexture'] = {'index': tex_index[tname]}
        gl['materials'].append(mat)
        gl['meshes'].append({'name': g.name, 'primitives': [{'attributes': attrs, 'indices': a_idx, 'material': len(gl['materials']) - 1}]})
        gl['nodes'].append({'mesh': len(gl['meshes']) - 1, 'name': g.name or hex(g.lo)})
        gl['scenes'][0]['nodes'].append(len(gl['nodes']) - 1)
    if info:
        return
    for k in ('textures', 'images'):
        if not gl[k]:
            del gl[k]
    while len(blob) % 4:
        blob.append(0)
    gl['buffers'].append({'byteLength': len(blob)})
    js = json.dumps(gl).encode()
    js += b' ' * (-len(js) % 4)
    out = b'glTF' + struct.pack('<II', 2, 12 + 8 + len(js) + 8 + len(blob))
    out += struct.pack('<I', len(js)) + b'JSON' + js + struct.pack('<I', len(blob)) + b'BIN\0' + bytes(blob)
    open(sys.argv[3], 'wb').write(out)
    print('->', sys.argv[3], len(out), 'bytes')


if __name__ == '__main__':
    main()
