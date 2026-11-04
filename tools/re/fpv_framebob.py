"""Test tool: the sway and bob as each frame drew it (the test pipe's "fpv framebob 1 <frames>": FirstPerson: framebob
lines) - its step a frame and how unevenly it steps (its change frame to frame: 0 for a smooth motion), in the arms'
metres and as degrees of the view at the gun hand's distance.

  fpv_framebob.py <log> [<distance m, default 0.4>]
"""
import math
import re
import sys

pat = re.compile(r'FirstPerson: framebob ([\d.]+) - at the frame (\S+) (\S+),')
rows = [[float(v) for v in m.groups()] for m in (pat.search(l) for l in open(sys.argv[1], errors='replace')) if m]
dist = float(sys.argv[2]) if len(sys.argv) > 2 else 0.4
deg = lambda m: math.degrees(m / dist)
steps = sorted(math.hypot(b[1] - a[1], b[2] - a[2]) for a, b in zip(rows, rows[1:]))
acc = sorted(math.hypot(c[1] - 2 * b[1] + a[1], c[2] - 2 * b[2] + a[2]) for a, b, c in zip(rows, rows[1:], rows[2:]))
xs, ys = [r[1] for r in rows], [r[2] for r in rows]
print('%d frames: range %.4f x %.4f m (%.2f x %.2f deg); step median %.3f deg p95 %.3f; unevenness median %.4f deg '
      'p95 %.4f max %.4f (%.0f%% of the median step at p95)' % (
          len(rows), max(xs) - min(xs), max(ys) - min(ys), deg(max(xs) - min(xs)), deg(max(ys) - min(ys)),
          deg(steps[len(steps) // 2]), deg(steps[int(len(steps) * 0.95)]), deg(acc[len(acc) // 2]),
          deg(acc[int(len(acc) * 0.95)]), deg(acc[-1]), 100 * acc[int(len(acc) * 0.95)] / steps[len(steps) // 2]))
