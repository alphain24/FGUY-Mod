"""Research: a motion recorded in the game (motion.py's format) as a glTF animation on a rigged character from ho2glb.

  motion2glb.py <rigged.glb> <motion.bin> <out.glb> [--name walk]

The game's pose has one bone more than the skeleton: its bone 0 is the character's root (the entity's origin); its
bone n + 1 is the skeleton's bone n (ho2glb's node "bone<n>"). Rotations, translations and scales are local, in the
parent's space - glTF's animation channels as they are."""
import json
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import motion  # noqa: E402


def read_glb(path):
    d = open(path, 'rb').read()
    n = struct.unpack_from('<I', d, 12)[0]
    gl = json.loads(d[20:20 + n])
    o = 20 + n
    blen = struct.unpack_from('<I', d, o)[0]
    return gl, bytearray(d[o + 8:o + 8 + blen])


def write_glb(path, gl, bin_):
    while len(bin_) % 4:
        bin_ += b'\0'
    gl['buffers'] = [{'byteLength': len(bin_)}]
    js = json.dumps(gl, separators=(',', ':')).encode()
    while len(js) % 4:
        js += b' '
    out = struct.pack('<III', 0x46546C67, 2, 12 + 8 + len(js) + 8 + len(bin_))
    out += struct.pack('<II', len(js), 0x4E4F534A) + js + struct.pack('<II', len(bin_), 0x004E4942) + bytes(bin_)
    open(path, 'wb').write(out)


def add(gl, bin_, data, kind, count, minmax=False, target=None):
    while len(bin_) % 4:
        bin_ += b'\0'
    raw = struct.pack('<%df' % len(data), *data)
    view = {'buffer': 0, 'byteOffset': len(bin_), 'byteLength': len(raw)}
    bin_ += raw
    gl.setdefault('bufferViews', []).append(view)
    acc = {'bufferView': len(gl['bufferViews']) - 1, 'componentType': 5126, 'count': count, 'type': kind}
    if minmax:
        acc['min'], acc['max'] = [min(data)], [max(data)]
    gl['accessors'].append(acc)
    return len(gl['accessors']) - 1


def main():
    src, mot, out = sys.argv[1:4]
    name = sys.argv[sys.argv.index('--name') + 1] if '--name' in sys.argv else os.path.splitext(os.path.basename(mot))[0]
    gl, bin_ = read_glb(src)
    bones, frames = motion.load(mot)
    nodes = {n.get('name'): i for i, n in enumerate(gl['nodes'])}
    t0 = frames[0][0]
    times = [t - t0 for t, _ in frames]
    a_time = add(gl, bin_, times, 'SCALAR', len(times), minmax=True)
    anim = {'name': name, 'samplers': [], 'channels': []}
    for g in range(1, bones):
        node = nodes.get('bone%02d' % (g - 1))
        if node is None:
            continue
        for path, lo, hi, kind in (('rotation', 0, 4, 'VEC4'), ('translation', 4, 7, 'VEC3'), ('scale', 7, 10, 'VEC3')):
            vals = [x for _, pose in frames for x in pose[g][lo:hi]]
            anim['samplers'].append({'input': a_time, 'output': add(gl, bin_, vals, kind, len(frames)),
                                     'interpolation': 'LINEAR'})
            anim['channels'].append({'sampler': len(anim['samplers']) - 1, 'target': {'node': node, 'path': path}})
    gl.setdefault('animations', []).append(anim)
    write_glb(out, gl, bin_)
    print(f'{out}: animation "{name}", {len(frames)} frames, {times[-1]:.2f} s, {len(anim["channels"])} channels')


if __name__ == '__main__':
    main()
