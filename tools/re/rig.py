"""Research: a character's skeleton (SkeletonBlob) and its pieces' vertex weights.

  rig.py <file.ho> <model name> [--check]

SkeletonBlob (PC, little endian): +4 u16 bone count; +0x14 offset of the parent table (a byte a bone, 0xff: root),
+0x18 offset of the mirror table (the bone on the other side, 0xff: none), +0x20 offset of the matrices: a 3x4 row-major
matrix a bone (translation in each row's 4th value) - the inverse bind pose (model space -> the bone's space).
A vertex's bones: its last dword - moved by one bone, a u16: that bone's matrix offset (bone * 48); by more, its bytes
are the bones, and the dword before it the weights (bytes adding up to 255)."""
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import ho  # noqa: E402
import ho2glb  # noqa: E402

T_SKEL = 0x9E3F4786


class Skeleton:
    def __init__(self, d):
        self.count = struct.unpack_from('<H', d, 4)[0]
        par_at, mir_at = struct.unpack_from('<II', d, 0x14)
        mat_at = struct.unpack_from('<I', d, 0x20)[0]
        self.parent = [b if b != 0xff else -1 for b in d[par_at:par_at + self.count]]
        self.mirror = [b if b != 0xff else -1 for b in d[mir_at:mir_at + self.count]]
        self.inv_bind = []
        for i in range(self.count):
            f = struct.unpack_from('<12f', d, mat_at + i * 48)
            self.inv_bind.append([list(f[0:4]), list(f[4:8]), list(f[8:12])])
        self.bind = [invert(m) for m in self.inv_bind]

    def position(self, i):
        return [self.bind[i][r][3] for r in range(3)]


def invert(m):
    """A 3x4 rigid (or near) transform's inverse: R^T, -R^T t (scale, if any, kept)."""
    r = [[m[i][j] for j in range(3)] for i in range(3)]
    t = [m[i][3] for i in range(3)]
    # general 3x3 inverse
    a, b, c = r[0]; d, e, f = r[1]; g, h, i = r[2]
    det = a * (e * i - f * h) - b * (d * i - f * g) + c * (d * h - e * g)
    inv = [[(e * i - f * h) / det, (c * h - b * i) / det, (b * f - c * e) / det],
           [(f * g - d * i) / det, (a * i - c * g) / det, (c * d - a * f) / det],
           [(d * h - e * g) / det, (b * g - a * h) / det, (a * e - b * d) / det]]
    ti = [-(inv[k][0] * t[0] + inv[k][1] * t[1] + inv[k][2] * t[2]) for k in range(3)]
    return [inv[k] + [ti[k]] for k in range(3)]


def skeleton_of(h, by, ids, model):
    sk = next(a for _, a in ho2glb.refs(model.data(), by, ids) if a.type == T_SKEL)
    return Skeleton(sk.data())


def vertex_bones(vd, layout):
    """[(offset, [(bone, weight)])] for each vertex (layout: ho2glb.vertex_layout)."""
    out = []
    for o, tangents, k in layout:
        end = o + 28 + (16 if tangents else 0) + (4 if k > 1 else 0)
        bones = vd[end - 4:end]
        if k == 1:   # (one bone: its matrix's byte offset in the bone table, 48 bytes a bone)
            out.append((o, [(struct.unpack_from('<H', bones, 0)[0] // 48, 1.0)]))
        else:
            w = vd[end - 8:end - 4]
            out.append((o, [(bones[j], w[j] / 255.0) for j in range(k) if j < 4]))
    return out


def main():
    h = ho.Ho(sys.argv[1])
    by = {(a.lo, a.hi): a for a in h.assets}
    ids = set(by)
    model = next(a for a in h.assets if a.type == ho2glb.T_MODEL and a.name == sys.argv[2])
    sk = skeleton_of(h, by, ids, model)
    print(model.name, sk.count, 'bones')
    for i in range(sk.count):
        p = sk.position(i)
        print(f'  {i:2} parent {sk.parent[i]:3} mirror {sk.mirror[i]:3} at ({p[0]:7.3f} {p[1]:7.3f} {p[2]:7.3f})')
    if '--check' not in sys.argv:
        return
    import math
    for g in [a for _, a in ho2glb.refs(model.data(), by, ids) if a.type == ho2glb.T_SKIN]:
        n, vb, uv, ib, mats = ho2glb.piece(g, by, ids)
        lay = ho2glb.vertex_layout(g.data(), n, vb.size) if vb else None
        if not lay:
            continue
        vd = vb.data()
        good = total = 0
        worst = 0
        for o, inf in vertex_bones(vd, lay):
            pos = struct.unpack_from('<3f', vd, o)
            b = max(inf, key=lambda x: x[1])[0]
            if b >= sk.count:
                total += 1
                continue
            d = math.dist(pos, sk.position(b))
            nearest = min(math.dist(pos, sk.position(j)) for j in range(sk.count))
            total += 1
            good += d <= nearest + 0.15
            worst = max(worst, d)
        print(f'piece {n:5} verts: main bone within 0.15 of the nearest bone for {good}/{total}; farthest {worst:.2f}')


if __name__ == '__main__':
    main()
