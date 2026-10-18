"""Research: the vtables of classes by (part of) their name - MSVC RTTI the other way round from vtname.py: the type
descriptor (".?AV<name>@@"), the object locators naming it (+0xc), the vtables just after a pointer to one.

  vtfind.py <exe> <name part> [...]
"""
import os
import re
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from pe import PE  # noqa: E402


def main():
    exe = PE(sys.argv[1])
    data = exe.data
    for part in sys.argv[2:]:
        for m in re.finditer(rb'\.\?AV[A-Za-z0-9_@?$]*' + re.escape(part.encode()) + rb'[A-Za-z0-9_@?$]*@@\0', data):
            name = m.group(0)[:-1].decode('latin1')
            td = exe.off2va(m.start() - 8)
            for c in re.finditer(re.escape(struct.pack('<I', td)), data):
                col = exe.off2va(c.start() - 12)
                off = struct.unpack_from('<I', data, c.start() - 8)[0]
                for v in re.finditer(re.escape(struct.pack('<I', col)), data):
                    print('%-60s vtable %08x%s' % (name, exe.off2va(v.start()) + 4, ' (at +%#x)' % off if off else ''))


if __name__ == '__main__':
    main()
