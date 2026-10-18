"""Research: how smoothly another PC shows a player turning. The player's own copy turns at a steady rate (the test pad's
right stick, held); the other PC's copy of them is sampled every few ms (its camera yaw, +0xf3b4) and its turning speed
from sample to sample is summed up: a smooth turn has a steady speed, a jerky one jumps between fast and still.

  aimsmooth.py <owner pid> <other pid> <player> [<seconds> [<stick>]]
"""
import math
import os
import statistics
import struct
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from aimat import send  # noqa: E402
from memwatch import players, reader  # noqa: E402


def wrap(a):
    while a > math.pi:
        a -= 2 * math.pi
    while a < -math.pi:
        a += 2 * math.pi
    return a


def main():
    owner, other, who = int(sys.argv[1]), int(sys.argv[2]), int(sys.argv[3])
    seconds = float(sys.argv[4]) if len(sys.argv) > 4 else 4
    stick = int(sys.argv[5]) if len(sys.argv) > 5 else 32000
    ro, rx = reader(owner), reader(other)
    po = [p for p in players(ro) if struct.unpack('<i', ro(p + 0xdf8, 4))[0] == who][0]
    px = [p for p in players(rx) if struct.unpack('<i', rx(p + 0xdf8, 4))[0] == who][0]
    yaw = lambda r, p: struct.unpack('<f', r(p + 0xf3b4, 4))[0]   # (the camera's: a player standing doesn't turn)
    send(owner, 'hold 0 %d rx=%d' % (int((seconds + 1) * 1000), stick))
    time.sleep(0.8)   # (up to speed)
    samples = []
    t0 = time.perf_counter()
    while time.perf_counter() - t0 < seconds:
        samples.append((time.perf_counter(), yaw(rx, px), yaw(ro, po)))
        time.sleep(0.004)
    def per_frame(col):   # (the turning speed from one shown frame to the next: samples where the value moved)
        pts, out = [], []
        for t, a, o in samples:
            v = a if col == 1 else o
            if not pts or v != pts[-1][1]:
                pts.append((t, v))
        for (t1, v1), (t2, v2) in zip(pts, pts[1:]):
            out.append(math.degrees(wrap(v2 - v1)) / (t2 - t1))
        gaps = [t2 - t1 for (t1, _), (t2, _) in zip(pts, pts[1:])]
        return out, gaps
    other_speed, other_gaps = per_frame(1)
    own_speed, own_gaps = per_frame(2)
    lag = [abs(math.degrees(wrap(a - o))) for _, a, o in samples]
    rel = lambda v: statistics.pstdev(v) / abs(statistics.mean(v)) if v and statistics.mean(v) else 0
    print('%d samples over %.1f s' % (len(samples), seconds))
    print('owner:    %4.0f deg/s, frame to frame spread %3.0f%%, %3d changes, longest gap %3.0f ms' % (
        statistics.mean(own_speed), rel(own_speed) * 100, len(own_speed), max(own_gaps) * 1000))
    print('other PC: %4.0f deg/s, frame to frame spread %3.0f%%, %3d changes, longest gap %3.0f ms, behind by %.1f deg' % (
        statistics.mean(other_speed), rel(other_speed) * 100, len(other_speed), max(other_gaps) * 1000,
        statistics.mean(lag)))


if __name__ == '__main__':
    main()
