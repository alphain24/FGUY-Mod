"""Usage: peekmouse.py <pid> - each controller device's header and its mouse fields (+0x454 x, +0x458 y,
+0x460, +0x468), plus the keyboard's controller index."""
import ctypes
import struct
import sys

k = ctypes.windll.kernel32
h = k.OpenProcess(0x0010 | 0x0400, False, int(sys.argv[1]))


def read(addr, n):
    buf = ctypes.create_string_buffer(n)
    got = ctypes.c_size_t()
    return buf.raw if k.ReadProcessMemory(h, ctypes.c_void_p(addr), buf, n, ctypes.byref(got)) else None


u32 = lambda a: struct.unpack('<I', read(a, 4))[0]
print('keyboard controller', u32(0xbad7f0))
table, count = u32(0xaefacc), u32(0xaf35ec)
for i in range(count):
    dev = u32(table + 4 * i)
    if not dev:
        continue
    head = struct.unpack('<8I', read(dev, 32))
    f = struct.unpack('<6i', read(dev + 0x454, 24))
    print(f'device {i} {dev:#x} head {[hex(x) for x in head]} mouse x {f[0]} y {f[1]} +45c {f[2]} +460 {f[3]} +464 {f[4]} +468 {f[5]} enabled {u32(dev + 0x6d0):#x}')
