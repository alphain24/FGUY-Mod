"""Research / tests: put a test copy's own player a few units from another player, its camera turned onto them (to see
that player on this PC in a screenshot).

  lookat.py <pid> <my player index> <target player index> [<distance>]
Uses the game's own player list (read only) and the copy's test pipe (needs [Debug] TestInput=1).
"""
import math
import os
import struct
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from aimat import send  # noqa: E402
from memwatch import players, reader  # noqa: E402


def main():
    pid, mine, target = int(sys.argv[1]), int(sys.argv[2]), int(sys.argv[3])
    dist = float(sys.argv[4]) if len(sys.argv) > 4 else 4
    read = reader(pid)
    lst = players(read)
    by = {struct.unpack('<i', read(p + 0xdf8, 4))[0]: (i, p) for i, p in enumerate(lst)}
    (li, me), (_, them) = by[mine], by[target]

    def pos(p):
        t = struct.unpack('<I', read(p + 0x38, 4))[0]
        return struct.unpack('<3f', read(t + 0x30, 12))

    a, b = pos(me), pos(them)
    dx, dz = a[0] - b[0], a[2] - b[2]
    d = math.hypot(dx, dz) or 1
    send(pid, 'teleport %d %.2f %.2f %.2f' % (li, b[0] + dx / d * dist, b[1] + 0.3, b[2] + dz / d * dist))
    time.sleep(0.4)
    for _ in range(10):
        a, b = pos(me), pos(them)
        send(pid, 'pokef %x %.4f' % (me + 0xf3b4, math.atan2(-(b[0] - a[0]), -(b[2] - a[2]))))
        time.sleep(0.05)


if __name__ == '__main__':
    main()
