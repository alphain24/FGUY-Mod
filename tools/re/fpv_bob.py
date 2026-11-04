"""Test tool: the game's own bounce as a character walks or runs (the test pipe's "fpv bobtrace <poses>": Motion: bob
lines - their chest and head against their place, each pose built). Per stretch of the log between two times: how far
the chest and head go up and down and side to side (the character's x: across, y: up, z: ahead), and how often.

  fpv_bob.py <log> [<from s> <to s>]  (times as the log's Motion: bob numbers)
"""
import re
import sys

pat = re.compile(r'Motion: bob ([\d.]+) - chest (\S+) (\S+) ([^\s,]+), head (\S+) (\S+) ([^\s,]+)')
rows = [[float(v) for v in m.groups()] for m in (pat.search(l) for l in open(sys.argv[1], errors='replace')) if m]
if len(sys.argv) > 3:
    rows = [r for r in rows if float(sys.argv[2]) <= r[0] <= float(sys.argv[3])]
t0 = rows[0][0]
span = rows[-1][0] - t0
print('%d poses over %.2f s (%.0f a second)' % (len(rows), span, len(rows) / span if span else 0))
for name, i in (('chest across', 1), ('chest up', 2), ('chest ahead', 3), ('head across', 4), ('head up', 5),
                ('head ahead', 6)):
    v = [r[i] for r in rows]
    mean = sum(v) / len(v)
    d = [x - mean for x in v]
    ups = sum(1 for a, b in zip(d, d[1:]) if a < 0 <= b)   # (upward crossings: one a cycle)
    # the step from pose to pose: smooth, or in jumps?
    steps = sorted(abs(b - a) for a, b in zip(v, v[1:]))
    print('%-13s %.3f..%.3f (%.3f peak to peak), %.1f cycles a second; pose to pose median %.4f, max %.4f' % (
        name, min(v), max(v), max(v) - min(v), ups / span if span else 0, steps[len(steps) // 2], steps[-1]))
