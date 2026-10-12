# Two PCs' random-number traces (FGUYMod\rng_<pid>.bin, FGUYMOD_RNGTRACE=1): the first tick where the game
# thread took different numbers, and from where.
#   python tools/rng_compare.py <rng_A.bin> <rng_B.bin>
import struct
import sys
from collections import Counter, defaultdict


def load(path):
    data = open(path, 'rb').read()
    ticks = defaultdict(list)
    for i in range(0, len(data) - 7, 8):
        tick, entry = struct.unpack_from('<II', data, i)
        ticks[tick].append((entry & 0x0fffffff, entry >> 28))
    return ticks


a, b = load(sys.argv[1]), load(sys.argv[2])
last = min(max(a) if a else 0, max(b) if b else 0)
print(f'A: {sum(map(len, a.values()))} numbers over ticks {min(a, default=0)}..{max(a, default=0)}; '
      f'B: {sum(map(len, b.values()))} over {min(b, default=0)}..{max(b, default=0)}; compared up to {last - 1}')
total = Counter()
for t in range(last):
    total.update(c for c, _ in a.get(t, []))
for t in range(last):   # (the last tick may be cut short in a file)
    x, y = a.get(t, []), b.get(t, [])
    if x != y:
        print(f'first difference at tick {t}: A took {len(x)} numbers, B {len(y)}')
        n = 0
        while n < min(len(x), len(y)) and x[n] == y[n]:
            n += 1
        print(f'  the same for the first {n}; then')
        print('  A:', ' '.join(f'{c:#x}/{w}' for c, w in x[n:n + 12]))
        print('  B:', ' '.join(f'{c:#x}/{w}' for c, w in y[n:n + 12]))
        ca, cb = Counter(c for c, _ in x), Counter(c for c, _ in y)
        diff = {c: (ca[c], cb[c]) for c in set(ca) | set(cb) if ca[c] != cb[c]}
        print('  callers with different counts (A, B):', ', '.join(f'{c:#x} {v}' for c, v in sorted(diff.items())))
        break
else:
    print('the same numbers on both, every tick')
print('busiest callers:', ', '.join(f'{c:#x} x{n}' for c, n in total.most_common(10)))
