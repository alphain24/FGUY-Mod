"""Research: switchkind.py <spot hex> <kind hex> <pid> [<pid> ...]: on each PC, the spot shows that kind instead of its own - the
kind's object gets the shown one's state bytes (+0xa0..+0xa3), the shown one is switched off (research)."""
import os, sys, struct
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from findobj import open_reader, find
from aimat import send

VT = 0xa8deb4
spot, kind = int(sys.argv[1], 16), int(sys.argv[2], 16)
for pid in map(int, sys.argv[3:]):
    h, r = open_reader(pid)
    objs = {}
    for a in find(h, r, VT):
        b = r(a, 0xb0)
        if not b or struct.unpack_from('<I', b, 0x18)[0] != spot:
            continue
        asset = struct.unpack_from('<I', b, 0x68)[0]
        objs[struct.unpack('<I', r(asset, 4))[0]] = (a, b)
    on = [k for k, (a, b) in objs.items() if b[0xa1] and b[0xa3]]
    if kind not in objs or not on:
        print(pid, 'kinds here', ['%x' % k for k in objs], 'showing', ['%x' % k for k in on]); continue
    a_on, b_on = objs[on[0]]
    a_new, b_new = objs[kind]
    state = struct.unpack_from('<I', b_on, 0xa0)[0]
    send(pid, 'poke %x %x' % (a_new + 0xa0, state))
    send(pid, 'poke %x %x' % (a_on + 0xa0, state & 0xff))
    print(pid, 'spot %x: %x off, %x on (state %08x)' % (spot, on[0], kind, state))
