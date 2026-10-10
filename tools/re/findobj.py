"""Research: find objects of a class in a running FGUY.exe by their vtable (read only).

  findobj.py <pid> <vtable hex> [<dump bytes>]     lists every object whose first dword is that vtable,
                                                    with its first <dump bytes> bytes as dwords (default 0x40)
"""
import ctypes
import struct
import sys

k32 = ctypes.WinDLL('kernel32', use_last_error=True)
k32.OpenProcess.restype = ctypes.c_void_p
k32.ReadProcessMemory.argtypes = [ctypes.c_void_p, ctypes.c_void_p, ctypes.c_void_p, ctypes.c_size_t,
                                  ctypes.POINTER(ctypes.c_size_t)]
k32.VirtualQueryEx.argtypes = [ctypes.c_void_p, ctypes.c_void_p, ctypes.c_void_p, ctypes.c_size_t]
k32.VirtualQueryEx.restype = ctypes.c_size_t


class MBI(ctypes.Structure):  # MEMORY_BASIC_INFORMATION as this (64-bit) Python sees it
    _fields_ = [('BaseAddress', ctypes.c_uint64), ('AllocationBase', ctypes.c_uint64), ('AllocationProtect', ctypes.c_uint32),
                ('PartitionId', ctypes.c_uint32), ('RegionSize', ctypes.c_uint64), ('State', ctypes.c_uint32),
                ('Protect', ctypes.c_uint32), ('Type', ctypes.c_uint32), ('_pad', ctypes.c_uint32)]


def open_reader(pid):
    h = k32.OpenProcess(0x0010 | 0x0400, False, pid)
    if not h:
        sys.exit('cannot open process')

    def read(addr, n):
        buf = ctypes.create_string_buffer(n)
        got = ctypes.c_size_t()
        if not k32.ReadProcessMemory(h, ctypes.c_void_p(addr), buf, n, ctypes.byref(got)):
            return None
        return buf.raw[:got.value]
    return h, read


def regions(h):
    addr = 0x10000
    while addr < 0x7fff0000:
        mbi = MBI()
        if not k32.VirtualQueryEx(h, ctypes.c_void_p(addr), ctypes.byref(mbi), ctypes.sizeof(mbi)):
            break
        if mbi.State == 0x1000 and mbi.Protect in (0x04, 0x08, 0x40, 0x80):  # committed, writable (heap or the exe data)
            yield mbi.BaseAddress, mbi.RegionSize
        addr = mbi.BaseAddress + mbi.RegionSize


def find(h, read, vtable):
    needle = struct.pack('<I', vtable)
    for base, size in regions(h):
        data = read(base, size)
        if not data:
            continue
        i = data.find(needle)
        while i != -1:
            if i % 4 == 0:
                yield base + i
            i = data.find(needle, i + 1)


def main():
    pid, vt = int(sys.argv[1]), int(sys.argv[2], 16)
    n = int(sys.argv[3], 16) if len(sys.argv) > 3 else 0x40
    h, read = open_reader(pid)
    for a in find(h, read, vt):
        d = read(a, n)
        words = struct.unpack('<%dI' % (len(d) // 4), d)
        print(f'{a:#010x}: ' + ' '.join(f'{w:08x}' for w in words))


if __name__ == '__main__':
    main()
