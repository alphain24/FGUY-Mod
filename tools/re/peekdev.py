"""Usage: peekdev.py <pid> <controller> - the game's device for a controller and its key array (VK, +0x484)."""
import ctypes
import struct
import sys

k = ctypes.windll.kernel32
h = k.OpenProcess(0x0010 | 0x0400, False, int(sys.argv[1]))


def read(addr, n):
    buf = ctypes.create_string_buffer(n)
    got = ctypes.c_size_t()
    if not k.ReadProcessMemory(h, ctypes.c_void_p(addr), buf, n, ctypes.byref(got)):
        return None
    return buf.raw


table = struct.unpack('<I', read(0xaefacc, 4))[0]
idx = int(sys.argv[2])
dev = struct.unpack('<I', read(table + 4 * idx, 4))[0]
print(f'table {table:#x}, device {idx} at {dev:#x}, enabled {struct.unpack("<I", read(dev + 0x6d0, 4))[0]}, '
      f'type {struct.unpack("<I", read(dev + 4, 4))[0]}')
keys = read(dev + 0x484, 256)
print('keys down:', [i for i, b in enumerate(keys) if b & 0x80])
prev = read(dev + 0x584, 256)
print('previous :', [i for i, b in enumerate(prev) if b & 0x80])
