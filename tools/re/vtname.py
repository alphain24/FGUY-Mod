"""Research: the C++ class name of vtables (MSVC RTTI: vtable[-1] = the object locator, +0xc its type descriptor,
whose name starts at +8).

  vtname.py <exe> <vtable hex> [...]       or   vtname.py <exe> -  (vtables from a vtcount.py diff on stdin)
"""
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from pe import PE  # noqa: E402


def name_of(exe, vt):
    try:
        col = struct.unpack('<I', exe.data[exe.va2off(vt - 4):exe.va2off(vt - 4) + 4])[0]
        td = struct.unpack('<I', exe.data[exe.va2off(col + 12):exe.va2off(col + 12) + 4])[0]
        off = struct.unpack('<I', exe.data[exe.va2off(col + 4):exe.va2off(col + 4) + 4])[0]
        raw = exe.data[exe.va2off(td + 8):exe.va2off(td + 8) + 120].split(b'\0')[0].decode('latin1')
        return raw + ('' if not off else ' (at +%#x)' % off)
    except Exception:
        return '?'


def main():
    exe = PE(sys.argv[1])
    if sys.argv[2] == '-':
        for line in sys.stdin:
            vt = int(line.split(':')[0], 16)
            print('%s  %s' % (line.rstrip(), name_of(exe, vt)))
    else:
        for v in sys.argv[2:]:
            print('%s  %s' % (v, name_of(exe, int(v, 16))))


if __name__ == '__main__':
    main()
