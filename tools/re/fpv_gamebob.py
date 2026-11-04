"""Test tool: the gun's bob as given (the test pipe's "fpv bobtrace <poses> 1": Motion: gamebob lines - the offset, the
view's left and up, each pose). Between two times: how far it goes and how often, and how smoothly (pose to pose).

  fpv_gamebob.py <log> [<from s> <to s>]
"""
import re
import sys

pat = re.compile(r'Motion: gamebob ([\d.]+) (\S+) (\S+)')
rows = [[float(v) for v in m.groups()] for m in (pat.search(l) for l in open(sys.argv[1], errors='replace')) if m]
if len(sys.argv) > 3:
    rows = [r for r in rows if float(sys.argv[2]) <= r[0] <= float(sys.argv[3])]
span = rows[-1][0] - rows[0][0]
print('%d poses over %.2f s' % (len(rows), span))
for name, i in (('side', 1), ('up', 2)):
    v = [r[i] for r in rows]
    mean = sum(v) / len(v)
    ups = sum(1 for a, b in zip(v, v[1:]) if a < mean <= b)
    st = sorted(abs(b - a) for a, b in zip(v, v[1:]))
    acc = sorted(abs(c - 2 * b + a) for a, b, c in zip(v, v[1:], v[2:]))
    print('%-5s %.4f..%.4f (%.4f peak to peak), %.1f a second; pose to pose median %.5f max %.5f; '
          'unevenness p95 %.5f max %.5f' % (name, min(v), max(v), max(v) - min(v), ups / span if span else 0,
                                           st[len(st) // 2], st[-1], acc[int(len(acc) * 0.95)], acc[-1]))
