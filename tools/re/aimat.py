"""Research / tests: a test copy's player 0 shoots at another player - teleported a few units from them, its camera
turned onto them every 50 ms (pipe "pokef" on the camera's yaw) while the trigger is held (pipe "hold").

  aimat.py <pid> [<target player>] [<seconds>] [<distance>] [<my player>]
Uses the game's own player list (read only) and the copy's test pipe (needs [Debug] TestInput=1).
"""
import math
import os
import struct
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from memwatch import players, reader  # noqa: E402


def send(pid, line):
    for _ in range(100):   # (the pipe takes one connection at a time: busy for a moment after each)
        try:
            with open('\\\\.\\pipe\\FGUYMod_%d' % pid, 'w') as p:
                p.write(line + '\n')
            return
        except OSError:
            time.sleep(0.005)


def main():
    pid = int(sys.argv[1])
    target = int(sys.argv[2]) if len(sys.argv) > 2 else 1
    seconds = float(sys.argv[3]) if len(sys.argv) > 3 else 3
    dist = float(sys.argv[4]) if len(sys.argv) > 4 else 6
    mine = int(sys.argv[5]) if len(sys.argv) > 5 else 0   # (the copy's own player: 0 on the host)
    read = reader(pid)

    def by_index():
        out = {}
        for i, p in enumerate(players(read)):
            out[struct.unpack('<i', read(p + 0xdf8, 4))[0]] = (i, p)
        return out

    def pos(p):
        t = struct.unpack('<I', read(p + 0x38, 4))[0]
        return struct.unpack('<3f', read(t + 0x30, 12))

    ps = by_index()
    (li, me), (_, them) = ps[mine], ps[target]
    a, b = pos(me), pos(them)
    dx, dz = a[0] - b[0], a[2] - b[2]
    d = math.hypot(dx, dz) or 1
    spot = (b[0] + dx / d * dist, b[1] + 0.3, b[2] + dz / d * dist)
    send(pid, 'teleport %d %.2f %.2f %.2f' % ((li,) + spot))
    time.sleep(0.4)
    until = time.time() + seconds
    k = 0
    while time.time() < until:
        a, b = pos(me), pos(them)
        yaw = math.atan2(-(b[0] - a[0]), -(b[2] - a[2]))
        send(pid, 'pokef %x %.4f' % (me + 0xf3b4, yaw))
        if k % 5 == 0:   # a trigger pull every 250 ms (some guns fire once per pull)
            send(pid, 'hold 0 120 rt=255')
        k += 1
        time.sleep(0.05)
    print('shot at player %d from %.1f units for %.1f s' % (target, dist, seconds))


if __name__ == '__main__':
    main()
