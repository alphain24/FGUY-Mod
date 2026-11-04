"""Test tool: how evenly the first-person gun moves on the screen - fpv_burst.py's tracking (a colour's centre, each
frame of a burst) summed up: its range, its step a frame, and how unevenly it steps (the step's change frame to frame:
0 for a smooth motion; a frame standing still then one moving twice as far is a jerk).

  fpv_drill.py <burst folder> [<more folders>...]
"""
import os
import subprocess
import sys

here = os.path.dirname(os.path.abspath(__file__))
COLOR = '177,71,2'   # (Peter's drill's orange in the test arena's light)
for folder in sys.argv[1:]:
    out = subprocess.run([sys.executable, os.path.join(here, 'fpv_burst.py'), folder, '--color', COLOR, '--tol', '90'], capture_output=True, text=True).stdout
    pts = []
    for line in out.splitlines():
        c = line.split()
        if len(c) >= 6 and c[0].isdigit() and c[2] != '-':
            pts.append((float(c[2]), float(c[3])))
    if len(pts) < 3:
        print(folder, 'no gun found')
        continue
    steps = sorted(((b[0] - a[0]) ** 2 + (b[1] - a[1]) ** 2) ** 0.5 for a, b in zip(pts, pts[1:]))
    acc = sorted(((c[0] - 2 * b[0] + a[0]) ** 2 + (c[1] - 2 * b[1] + a[1]) ** 2) ** 0.5
                 for a, b, c in zip(pts, pts[1:], pts[2:]))
    xs, ys = [p[0] for p in pts], [p[1] for p in pts]
    print('%-40s range %.1f x %.1f px; step median %.3f p95 %.3f; unevenness median %.3f p95 %.3f max %.3f'
          % (os.path.basename(folder), max(xs) - min(xs), max(ys) - min(ys), steps[len(steps) // 2],
             steps[int(len(steps) * 0.95)], acc[len(acc) // 2], acc[int(len(acc) * 0.95)], acc[-1]))
