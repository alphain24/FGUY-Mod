"""Usage: imports.py <exe> <name filter> [--calls]
Lists import address table slots whose function name contains the filter; with --calls, also every
`call [slot]` in the exe with the code just before it."""
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from pe import PE  # noqa: E402

e = PE(sys.argv[1])
d = e.data
pe = struct.unpack_from('<I', d, 0x3C)[0]
imp_rva = struct.unpack_from('<I', d, pe + 24 + 104)[0]
o = e.va2off(e.imagebase + imp_rva)
slots = {}
while True:
    oft, _, _, name, ft = struct.unpack_from('<IIIII', d, o)
    if not name:
        break
    t, i = oft or ft, 0
    while True:
        ent = struct.unpack_from('<I', d, e.va2off(e.imagebase + t) + 4 * i)[0]
        if not ent:
            break
        if not ent & 0x80000000:
            fn = e.cstr_at(e.imagebase + ent + 2) or ''
            if sys.argv[2] in fn:
                slots[fn] = e.imagebase + ft + 4 * i
        i += 1
    o += 20
for fn, va in sorted(slots.items()):
    print(f'{va:08x}  {fn}')
    if '--calls' in sys.argv:
        for r in e.find_bytes(b'\xff\x15' + struct.pack('<I', va)):
            at = e.off2va(r)
            print(f'  call at {at:08x}')
            print(e.disasm(at, count=3, before=32))
