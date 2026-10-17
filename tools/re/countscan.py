"""Research: find a counter in a running game by how it changes (like a cheat finder) - snapshots of every writable
heap region, narrowed after each step to the dwords that changed by the step's amount.

  countscan.py <pid> <step> [<step> ...]
A step is "<+n or -n>:<command for the pipe, ';' between several>:<seconds to wait>", e.g.
  countscan.py 1234 "+1:teleport 0 9.8 9.4 184.7:2" "-1:hold 0 200 down:3"
(the first snapshot is taken before the first step). Prints the addresses left after each step.
"""
import os
import subprocess
import sys
import time

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from findobj import open_reader, regions  # noqa: E402

IMAGE = (0x400000, 0xc51000)
PIPE = os.path.join(os.path.dirname(os.path.abspath(__file__)), 'pipe.ps1')


def snap(h, read):
    out = {}
    for base, size in regions(h):
        if IMAGE[0] <= base < IMAGE[1] and base < 0xac1000:
            continue
        b = read(base, size)
        if b:
            out[base] = np.frombuffer(b[:len(b) // 4 * 4], dtype='<i4').copy()
    return out


def main():
    pid = int(sys.argv[1])
    h, read = open_reader(pid)
    prev = snap(h, read)
    cand = None   # {base: index array}
    for step in sys.argv[2:]:
        delta, cmds, wait = step.split(':', 2)
        for c in cmds.split(';'):
            if c.strip():
                subprocess.run(['powershell', '-File', PIPE, '-Id', str(pid)] + c.split(), check=False)
        time.sleep(float(wait))
        now = snap(h, read)
        d = int(delta)
        nxt = {}
        for base, w in now.items():
            p = prev.get(base)
            if p is None or len(p) != len(w):
                continue
            idx = np.arange(len(w)) if cand is None else cand.get(base)
            if idx is None or not len(idx):
                continue
            keep = idx[(w[idx] - p[idx] == d) & (w[idx] >= 0) & (w[idx] < 100)]
            if len(keep):
                nxt[base] = keep
        cand, prev = nxt, now
        n = sum(len(v) for v in cand.values())
        print('after %s: %d left' % (step, n), flush=True)
        if n <= 40:
            for base, idx in sorted(cand.items()):
                for i in idx:
                    print('  %08x = %d' % (base + 4 * int(i), now[base][i]))


if __name__ == '__main__':
    main()
