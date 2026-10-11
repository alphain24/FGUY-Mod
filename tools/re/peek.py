"""Usage: peek.py <pid> <addr> [<addr> ...] - reads a 32-bit value (and the first byte) at each address."""
import ctypes
import struct
import sys

k = ctypes.windll.kernel32
h = k.OpenProcess(0x0010 | 0x0400, False, int(sys.argv[1]))
for a in sys.argv[2:]:
    addr = int(a, 16)
    buf = ctypes.create_string_buffer(4)
    n = ctypes.c_size_t()
    ok = k.ReadProcessMemory(h, ctypes.c_void_p(addr), buf, 4, ctypes.byref(n))
    v = struct.unpack('<I', buf.raw)[0] if ok else None
    print(f'{addr:08x}: ' + (f'{v:#010x} ({struct.unpack("<i", buf.raw)[0]})' if ok else 'unreadable'))
