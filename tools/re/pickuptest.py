"""Research: which of a running level's floating collectibles (pickups, vtable 0xa8deb4) does player 0 take, and what
changes in them? Player 0's health is set low first, then it's teleported onto each active pickup in turn.

  pickuptest.py <pid> <pickups.pkl>      (the list of active pickups: [(address, position), ...], see the notes)
"""
import os
import pickle
import struct
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from aimat import send  # noqa: E402
from memwatch import players, reader  # noqa: E402


def main():
    pid = int(sys.argv[1])
    act = pickle.load(open(sys.argv[2], 'rb'))
    read = reader(pid)
    me = players(read)[0]
    health = struct.unpack('<I', read(me + 0x105c, 4))[0] + 0x50
    send(pid, 'pokef %x 50' % health)
    time.sleep(0.3)
    for a, pos in act:
        b0 = read(a, 0xe0)
        hp0 = struct.unpack('<f', read(health, 4))[0]
        send(pid, 'teleport 0 %.2f %.2f %.2f' % (pos[0], pos[1] + 0.2, pos[2]))
        time.sleep(0.8)
        b1 = read(a, 0xe0)
        hp1 = struct.unpack('<f', read(health, 4))[0]
        changes = ['%#x %08x->%08x' % (i, struct.unpack_from('<I', b0, i)[0], struct.unpack_from('<I', b1, i)[0])
                   for i in range(0, 0xe0, 4) if b0[i:i + 4] != b1[i:i + 4] and i not in (0x4c, 0xac)]
        print('%#x id %#x health %.0f -> %.0f  %s' % (a, struct.unpack_from('<I', b0, 0x18)[0], hp0, hp1, changes))


if __name__ == '__main__':
    main()
