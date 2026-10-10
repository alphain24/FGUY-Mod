"""Research: which fields of a running game's player objects change? Reads FGUY.exe's memory (read only).

  memwatch.py <pid> snap <file>        save player 0's object (and the objects it points to) to <file>
  memwatch.py <pid> diff <file>        compare now against <file>: list float triples that moved
The player list is the game's own (0xbad988: pointers, count at +0x20); player 0 = its first entry.
"""
import ctypes
import pickle
import struct
import sys

k32 = ctypes.WinDLL('kernel32', use_last_error=True)
k32.OpenProcess.restype = ctypes.c_void_p
k32.ReadProcessMemory.argtypes = [ctypes.c_void_p, ctypes.c_void_p, ctypes.c_void_p, ctypes.c_size_t,
                                  ctypes.POINTER(ctypes.c_size_t)]


def reader(pid):
    h = k32.OpenProcess(0x0010 | 0x0400, False, pid)  # VM_READ | QUERY_INFORMATION
    if not h:
        sys.exit('cannot open process')

    def read(addr, n):
        buf = ctypes.create_string_buffer(n)
        got = ctypes.c_size_t()
        if not k32.ReadProcessMemory(h, ctypes.c_void_p(addr), buf, n, ctypes.byref(got)):
            return None
        return buf.raw[:got.value]
    return read


def players(read):
    lst = struct.unpack('<I', read(0xbad988, 4))[0]
    n = struct.unpack('<i', read(lst + 0x20, 4))[0]
    return [struct.unpack('<I', read(lst + 4 * i, 4))[0] for i in range(min(n, 8))]


def player0(read):
    return players(read)[0]


def snapshot(read):
    blocks = {}
    for pi, p in enumerate(players(read)):
        obj = read(p, 0x1000)
        if not obj:
            continue
        blocks[(f'p{pi}', 0)] = obj
        for off in range(0, len(obj), 4):  # one level of pointers
            v = struct.unpack_from('<I', obj, off)[0]
            if 0x01000000 <= v < 0x7fff0000 and v % 4 == 0:
                b = read(v, 0x400)
                if b:
                    blocks[(f'p{pi}.ptr', off)] = b
    return blocks


def moved(a, b):
    out = []
    for off in range(0, min(len(a), len(b)) - 11, 4):
        x0, y0, z0 = struct.unpack_from('<3f', a, off)
        x1, y1, z1 = struct.unpack_from('<3f', b, off)
        vals = (x0, y0, z0, x1, y1, z1)
        if any(v != v or abs(v) > 1e5 for v in vals):
            continue
        d = ((x1 - x0) ** 2 + (z1 - z0) ** 2) ** 0.5
        if d > 0.5 and abs(y1 - y0) < 5 and abs(x0) + abs(z0) > 1 and abs(x0) < 5000 and abs(z0) < 5000:
            out.append((off, d, (x0, y0, z0), (x1, y1, z1)))
    return out


def main():
    pid, cmd, path = int(sys.argv[1]), sys.argv[2], sys.argv[3]
    read = reader(pid)
    snap = snapshot(read)
    if cmd == 'snap':
        pickle.dump(snap, open(path, 'wb'))
        print('players ' + ', '.join(f'{p:#x}' for p in players(read)) + f': {len(snap)} blocks saved')
        return
    old = pickle.load(open(path, 'rb'))
    for key in sorted(old):
        if key not in snap:
            continue
        for off, d, a, b in moved(old[key], snap[key]):
            print(f'{key[0]} {key[1]:#05x} +{off:#05x}: moved {d:7.2f}  '
                  f'({a[0]:.2f}, {a[1]:.2f}, {a[2]:.2f}) -> ({b[0]:.2f}, {b[1]:.2f}, {b[2]:.2f})')


if __name__ == '__main__':
    main()
