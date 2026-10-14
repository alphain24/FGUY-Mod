# Two PCs' enemy traces (FGUYMod\npc_<pid>.bin, FGUYMOD_RNGTRACE=1): the first enemy update that differs - its
# tick, its place in that tick's order, the time step it got and where it was.
#   python tools/npc_compare.py <npc_A.bin> <npc_B.bin>
import struct
import sys
from collections import defaultdict

REC = struct.Struct('<IIffff27I')


def load(path):
    data = open(path, 'rb').read()
    ticks = defaultdict(list)
    for i in range(0, len(data) - REC.size + 1, REC.size):
        v = REC.unpack_from(data, i)
        ticks[v[0]].append(v[2:])
    return ticks


a, b = load(sys.argv[1]), load(sys.argv[2])
last = min(max(a, default=0), max(b, default=0))
print(f'A: {sum(map(len, a.values()))} updates, B: {sum(map(len, b.values()))}; compared up to tick {last - 1}')
for t in range(last):
    x, y = a.get(t, []), b.get(t, [])
    if x == y:
        continue
    print(f'first difference at tick {t}: A updated {len(x)} enemies, B {len(y)}')
    for i in range(max(len(x), len(y))):
        ex = x[i] if i < len(x) else None
        ey = y[i] if i < len(y) else None
        if ex != ey:
            print(f'  #{i}: dt/pos A {ex[:4] if ex else None}\n           B {ey[:4] if ey else None}')
            if ex and ey:   # its effects timer (+0x860), word by word
                for w in range(27):
                    if ex[4 + w] != ey[4 + w]:
                        fa = struct.unpack('<f', struct.pack('<I', ex[4 + w]))[0]
                        fb = struct.unpack('<f', struct.pack('<I', ey[4 + w]))[0]
                        print(f'      timer +{w * 4:#04x}: A {ex[4 + w]:08x} ({fa:.6g})  B {ey[4 + w]:08x} ({fb:.6g})')
    prev = a.get(t - 1, [])
    print(f'  (tick {t - 1}: {len(prev)} enemies, the same on both)')
    break
else:
    print('no difference')
