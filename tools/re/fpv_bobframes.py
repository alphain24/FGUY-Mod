"""Test tool: the gun's bob as each frame shows it - the bob log (fpv bobtrace <n> 1) at a burst's frame times (both
the PC's clock, seconds): per frame the latest pose's bob, its step, and how evenly it steps; and the poses' times -
how often a new one came between two frames.

  fpv_bobframes.py <log> <burst folder>
"""
import re
import sys

pat = re.compile(r'Motion: gamebob ([\d.]+) (\S+) (\S+)')
poses = [[float(v) for v in m.groups()] for m in (pat.search(l) for l in open(sys.argv[1], errors='replace')) if m]
frames = [float(t) for t in open(sys.argv[2] + '\\times.txt')]
k = 0
shown, newPoses = [], []
for t in frames:
    n = 0
    while k + 1 < len(poses) and poses[k + 1][0] <= t:
        k += 1
        n += 1
    shown.append(poses[k][1:])
    newPoses.append(n)
steps = [((b[0] - a[0]) ** 2 + (b[1] - a[1]) ** 2) ** 0.5 for a, b in zip(shown, shown[1:])]
acc = sorted(((c[0] - 2 * b[0] + a[0]) ** 2 + (c[1] - 2 * b[1] + a[1]) ** 2) ** 0.5
             for a, b, c in zip(shown, shown[1:], shown[2:]))
st = sorted(steps)
print('%d frames; poses between two frames: %s' % (len(frames), ' '.join(str(c) for c in sorted(set(newPoses)))),
      '- frames with none:', newPoses.count(0))
print('bob step a frame: median %.5f p95 %.5f max %.5f; unevenness median %.5f p95 %.5f max %.5f; frames standing still %d'
      % (st[len(st) // 2], st[int(len(st) * 0.95)], st[-1], acc[len(acc) // 2], acc[int(len(acc) * 0.95)], acc[-1],
         sum(1 for s in steps if s < 1e-6)))
gaps = sorted(b[0] - a[0] for a, b in zip(poses, poses[1:]))
print('poses: %.0f a second; gap median %.1f ms, p95 %.1f, max %.1f; bob unchanged pose to pose %d of %d' % (
    len(poses) / (poses[-1][0] - poses[0][0]), gaps[len(gaps) // 2] * 1000, gaps[int(len(gaps) * 0.95)] * 1000,
    gaps[-1] * 1000, sum(1 for a, b in zip(poses, poses[1:]) if a[1:] == b[1:]), len(poses) - 1))
