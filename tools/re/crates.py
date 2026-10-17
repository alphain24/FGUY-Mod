"""Research: a running level's supply crates (the store) - every zInteraction (vtable 0xa8d6fc) with its id (+0x18), the
menu it opens (+0x28 -> text at +0xc, e.g. "=MNU_GLOBAL_SUPPLYCRATE"), and where it is (the zPlatform with that id).

  crates.py <pid>
"""
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from findobj import open_reader, find  # noqa: E402

INTERACTION, PLATFORM = 0xa8d6fc, 0xa19844


def main():
    pid = int(sys.argv[1])
    h, read = open_reader(pid)
    where = {}
    for a in find(h, read, PLATFORM):
        sid = struct.unpack('<I', read(a + 0x18, 4))[0]
        m = struct.unpack('<I', read(a + 0x38, 4))[0]
        b = read(m + 0x30, 12) if 0x10000 < m < 0x7fff0000 else None
        if b:
            where[sid] = struct.unpack('<3f', b)
    for a in find(h, read, INTERACTION):
        sid = struct.unpack('<I', read(a + 0x18, 4))[0]
        t = struct.unpack('<I', read(a + 0x28, 4))[0]
        text = (read(t + 0xc, 40) or b'').split(b'\0')[0].decode('latin1') if t else ''
        p = where.get(sid)
        print('%#x %-28s %s' % (sid, text, '(%.1f %.1f %.1f)' % p if p else '?'))


if __name__ == '__main__':
    main()
