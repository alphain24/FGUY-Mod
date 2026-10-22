"""Research: a motion recorded in the game (test pipe "poserec": FGUYMod\\motion_<slot>.bin) - and the skeleton's
bind pose, to compare.

  motion.py <motion.bin> [--frame N]                      - its bones' local pose (rotation x y z w, translation, scale)
  motion.py <motion.bin> --bind <file.ho> <model name>    - next to the skeleton's bind pose (local, from its matrices)

motion_<slot>.bin: "FGMO", version 1, bones, frames (int32s); a frame: its time (s), then a bone: rotation x y z w,
translation x y z, scale x y z (floats) - in its parent's space, as the game's animation hands it to the pose code."""
import math
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))


def load(path):
    d = open(path, 'rb').read()
    magic, version, bones, frames = struct.unpack_from('<4sIII', d, 0)
    assert magic == b'FGMO' and version == 1, 'not a motion'
    out, o = [], 16
    for _ in range(frames):
        t = struct.unpack_from('<f', d, o)[0]
        o += 4
        pose = [struct.unpack_from('<10f', d, o + b * 40) for b in range(bones)]
        o += bones * 40
        out.append((t, pose))
    return bones, out


def quat_of(m):
    """Rotation part of a 3x4 row-major matrix (columns: the bone's axes) as a quaternion x y z w."""
    r = [[m[i][j] for j in range(3)] for i in range(3)]
    s = [math.sqrt(sum(r[i][j] ** 2 for i in range(3))) for j in range(3)]
    r = [[r[i][j] / s[j] for j in range(3)] for i in range(3)]
    tr = r[0][0] + r[1][1] + r[2][2]
    if tr > 0:
        w = math.sqrt(1 + tr) / 2
        return ((r[2][1] - r[1][2]) / (4 * w), (r[0][2] - r[2][0]) / (4 * w), (r[1][0] - r[0][1]) / (4 * w), w)
    i = max(range(3), key=lambda k: r[k][k])
    j, k = (i + 1) % 3, (i + 2) % 3
    q = [0, 0, 0, 0]
    q[i] = math.sqrt(1 + r[i][i] - r[j][j] - r[k][k]) / 2
    q[j] = (r[j][i] + r[i][j]) / (4 * q[i])
    q[k] = (r[k][i] + r[i][k]) / (4 * q[i])
    q[3] = (r[k][j] - r[j][k]) / (4 * q[i])
    return tuple(q)


def main():
    bones, frames = load(sys.argv[1])
    n = int(sys.argv[sys.argv.index('--frame') + 1]) if '--frame' in sys.argv else 0
    t, pose = frames[n]
    print(f'{len(frames)} frames ({frames[-1][0] - frames[0][0]:.2f} s), {bones} bones; frame {n} at {t:.3f} s')
    bind = None
    if '--bind' in sys.argv:
        import ho
        import rig
        i = sys.argv.index('--bind')
        h = ho.Ho(sys.argv[i + 1])
        by = {(a.lo, a.hi): a for a in h.assets}
        model = next(a for a in h.assets if a.type == rig.ho2glb.T_MODEL and a.name == sys.argv[i + 2])
        sk = rig.skeleton_of(h, by, set(by), model)

        def mul(a, b):
            return [[sum(a[r][k] * b[k][c] for k in range(3)) + (a[r][3] if c == 3 else 0) for c in range(4)]
                    for r in range(3)]
        bind = []
        for b in range(sk.count):
            local = sk.bind[b] if sk.parent[b] < 0 else mul(sk.inv_bind[sk.parent[b]], sk.bind[b])
            bind.append((sk.parent[b], quat_of(local), [local[r][3] for r in range(3)]))
    for b, p in enumerate(pose):
        line = (f'{b:2}  rot {p[0]:7.3f} {p[1]:7.3f} {p[2]:7.3f} {p[3]:7.3f}  pos {p[4]:7.3f} {p[5]:7.3f} {p[6]:7.3f}'
                f'  scale {p[7]:5.2f} {p[8]:5.2f} {p[9]:5.2f}')
        if bind and b < len(bind):
            par, q, tr = bind[b]
            line += f'   | bind (parent {par:2}) rot {q[0]:7.3f} {q[1]:7.3f} {q[2]:7.3f} {q[3]:7.3f}  pos {tr[0]:7.3f} {tr[1]:7.3f} {tr[2]:7.3f}'
        print(line)


if __name__ == '__main__':
    main()
