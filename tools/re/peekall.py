"""Usage: peekall.py <pid> - window key table and every controller device's key array."""
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
win = u32(0xad39e8)
print('window keys:', [i for i, b in enumerate(read(win + 0xc2, 256)) if b & 0x80])
table, count = u32(0xaefacc), u32(0xaf35ec)
for i in range(count):
    dev = u32(table + 4 * i)
    if not dev:
        continue
    keys = read(dev + 0x484, 256)
    print(f'device {i} {dev:#x} vtable {u32(dev):#x} +6d0 {u32(dev + 0x6d0):#x} keys', [j for j, b in enumerate(keys) if b & 0x80] if keys else '?')
