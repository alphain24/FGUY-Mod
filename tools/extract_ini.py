"""Extract the game's built-in default config (resource type "INI", id 101) from FGUY.exe.

Usage: python extract_ini.py <path to FGUY.exe> <output file>
"""
import struct
import sys


def find_resource(exe, rtype, rid):
    pe = struct.unpack_from('<I', exe, 0x3C)[0]
    nsec, optsz = struct.unpack_from('<H', exe, pe + 6)[0], struct.unpack_from('<H', exe, pe + 20)[0]
    opt = pe + 24
    rsrc_rva = struct.unpack_from('<I', exe, opt + 96 + 8 * 2)[0]
    sections = []
    for i in range(nsec):
        o = opt + optsz + 40 * i
        vsize, va, rsize, rptr = struct.unpack_from('<IIII', exe, o + 8)
        sections.append((va, max(vsize, rsize), rptr))

    def rva2off(rva):
        return next(rva - va + rp for va, sz, rp in sections if va <= rva < va + sz)

    base = rva2off(rsrc_rva)

    def entries(off):
        named, ids = struct.unpack_from('<HH', exe, off + 12)
        for i in range(named + ids):
            name, target = struct.unpack_from('<II', exe, off + 16 + 8 * i)
            if name & 0x80000000:
                so = base + (name & 0x7fffffff)
                n = struct.unpack_from('<H', exe, so)[0]
                key = exe[so + 2:so + 2 + 2 * n].decode('utf-16le')
            else:
                key = name
            yield key, target

    for key, target in entries(base):
        if key != rtype:
            continue
        for key2, target2 in entries(base + (target & 0x7fffffff)):
            if key2 != rid:
                continue
            _, lang_target = next(entries(base + (target2 & 0x7fffffff)))
            data_rva, size = struct.unpack_from('<II', exe, base + lang_target)
            off = rva2off(data_rva)
            return exe[off:off + size]
    return None


if __name__ == '__main__':
    exe = open(sys.argv[1], 'rb').read()
    data = find_resource(exe, 'INI', 101)
    if data is None:
        sys.exit('INI resource 101 not found')
    open(sys.argv[2], 'wb').write(data)
    print(f'wrote {len(data)} bytes to {sys.argv[2]}')
