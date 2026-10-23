"""A rigged .glb (its skin's joints = the character's skeleton bones in order, as ho2glb / the Skin Studio export them)
-> a model pack (.fgmodel) for the FGUY Cosmetics mod: drawn in the game instead of that character (src/loader/models.cpp).

  glb2fgmodel.py <in.glb> <out.fgmodel> --name "Robot Stewie" --character Stewie --models 53,513 [43,513 ...]
                 [--tint ff8080]   (research: the textures tinted - to tell it from the game's own)

The pack: a zip, stored - model.json (name, character, "models": the outfits it replaces [[lo, hi], ...]), mesh.bin and
textures/<n>.png. mesh.bin, little endian:
  "FGMD", version 1, bones S, parts P, vertices V, indices I            (int32s)
  parents: S int8 (the skeleton's: -1 for a root)
  inverse bind matrices: S x 12 floats (3x4 row-major, the translation in each row's 4th)
  parts: P x (first index, index count, texture) (uint32s)
  vertices: V x (position 3, normal 3, uv 2 floats; bones 4 uint8; weights 4 uint8 adding up to 255)
  indices: I x uint16
"""
import json
import os
import struct
import sys
import zipfile


def read_glb(path):
    d = open(path, 'rb').read()
    n = struct.unpack_from('<I', d, 12)[0]
    gl = json.loads(d[20:20 + n])
    o = 20 + n
    blen = struct.unpack_from('<I', d, o)[0]
    return gl, d[o + 8:o + 8 + blen]


def accessor(gl, bin_, i):
    a = gl['accessors'][i]
    v = gl['bufferViews'][a['bufferView']]
    comp = {5126: ('f', 4), 5123: ('H', 2), 5121: ('B', 1), 5125: ('I', 4)}[a['componentType']]
    n = {'SCALAR': 1, 'VEC2': 2, 'VEC3': 3, 'VEC4': 4, 'MAT4': 16}[a['type']]
    start = v.get('byteOffset', 0) + a.get('byteOffset', 0)
    stride = v.get('byteStride', comp[1] * n)
    out = []
    for k in range(a['count']):
        out.append(struct.unpack_from('<%d%s' % (n, comp[0]), bin_, start + k * stride))
    return out


def main():
    args = sys.argv[1:]
    src, dst = args[0], args[1]
    opt = lambda k, d=None: args[args.index(k) + 1] if k in args else d
    name, character = opt('--name', 'Model'), opt('--character', '')
    models = []
    if '--models' in args:
        for x in args[args.index('--models') + 1:]:
            if x.startswith('--'):
                break
            lo, hi = x.split(',')
            models.append([int(lo, 0), int(hi, 0)])
    tint = opt('--tint')
    gl, bin_ = read_glb(src)
    skin = gl['skins'][0]
    joints = skin['joints']
    S = len(joints)
    ibm = accessor(gl, bin_, skin['inverseBindMatrices'])
    inv = []
    for m in ibm:   # (glTF: column-major 4x4) -> 3x4 row-major
        inv += [m[0], m[4], m[8], m[12], m[1], m[5], m[9], m[13], m[2], m[6], m[10], m[14]]
    parent = [-1] * S
    for i, j in enumerate(joints):
        for c in gl['nodes'][j].get('children', []):
            if c in joints:
                parent[joints.index(c)] = i
    verts, idx, parts = [], [], []
    textures = []   # (image index -> texture number)
    for node in gl['nodes']:
        if 'mesh' not in node or 'skin' not in node:
            continue
        for prim in gl['meshes'][node['mesh']]['primitives']:
            at = prim['attributes']
            if 'JOINTS_0' not in at:
                continue
            pos = accessor(gl, bin_, at['POSITION'])
            nrm = accessor(gl, bin_, at['NORMAL']) if 'NORMAL' in at else [(0, 1, 0)] * len(pos)
            uv = accessor(gl, bin_, at['TEXCOORD_0']) if 'TEXCOORD_0' in at else [(0, 0)] * len(pos)
            jn = accessor(gl, bin_, at['JOINTS_0'])
            wt = accessor(gl, bin_, at['WEIGHTS_0'])
            ind = [i[0] for i in accessor(gl, bin_, prim['indices'])]
            img = None
            mat = gl['materials'][prim['material']] if 'material' in prim else {}
            t = mat.get('pbrMetallicRoughness', {}).get('baseColorTexture')
            if t is not None:
                img = gl['textures'][t['index']]['source']
            if img not in textures:
                textures.append(img)
            base = len(verts)
            for k in range(len(pos)):
                w = list(wt[k])
                if any(isinstance(x, int) for x in w) and max(w) > 1:
                    w = [x / 255.0 for x in w]
                total = sum(w) or 1
                wb = [int(round(x / total * 255)) for x in w]
                wb[wb.index(max(wb))] += 255 - sum(wb)
                verts.append((pos[k], nrm[k], uv[k], [min(255, j) for j in jn[k]], wb))
            parts.append((len(idx), len(ind), textures.index(img)))
            idx += [base + i for i in ind]
    assert len(verts) < 65536, 'too many vertices for 16-bit triangles'
    out = bytearray(b'FGMD' + struct.pack('<5i', 1, S, len(parts), len(verts), len(idx)))
    out += struct.pack('<%db' % S, *parent)
    out += struct.pack('<%df' % len(inv), *inv)
    for p in parts:
        out += struct.pack('<3I', *p)
    for p, n, u, j, w in verts:
        out += struct.pack('<8f4B4B', *p, *n, *u, *j, *w)
    out += struct.pack('<%dH' % len(idx), *idx)
    files = {'mesh.bin': bytes(out)}
    pngs = []
    for k, img in enumerate(textures):
        data = b''
        if img is not None:
            im = gl['images'][img]
            v = gl['bufferViews'][im['bufferView']]
            data = bin_[v.get('byteOffset', 0):v.get('byteOffset', 0) + v['byteLength']]
            if tint:
                import io
                from PIL import Image, ImageChops
                pic = Image.open(io.BytesIO(data)).convert('RGBA')
                col = Image.new('RGBA', pic.size, '#' + tint + 'ff')
                pic = ImageChops.multiply(pic, col)
                b = io.BytesIO()
                pic.save(b, 'PNG')
                data = b.getvalue()
        files['textures/%d.png' % k] = data
        pngs.append('textures/%d.png' % k)
    manifest = {'format': 1, 'name': name, 'character': character, 'models': models, 'bones': S + 1, 'textures': pngs,
                'tool': 'glb2fgmodel.py'}
    with zipfile.ZipFile(dst, 'w', zipfile.ZIP_STORED) as z:
        z.writestr('model.json', json.dumps(manifest))
        for k, v in files.items():
            z.writestr(k, v)
    print(f'{dst}: {len(verts)} vertices, {len(idx) // 3} triangles, {len(parts)} parts, {len(textures)} textures, {S} bones')


if __name__ == '__main__':
    main()
