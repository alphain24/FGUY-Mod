# Disassembles the game exe (read-only) around addresses: python tools/disasm.py <hex start> [count] [exe]
import struct
import sys

import capstone

exe = sys.argv[3] if len(sys.argv) > 3 else 'A:/Games/Activision/Family Guy Back to the Multiverse/FGUY.exe'
d = open(exe, 'rb').read()
pe = struct.unpack_from('<I', d, 0x3c)[0]
n = struct.unpack_from('<H', d, pe + 6)[0]
opt = struct.unpack_from('<H', d, pe + 20)[0]
base = struct.unpack_from('<I', d, pe + 52)[0]
secs = [struct.unpack_from('<8sIIII', d, pe + 24 + opt + i * 40) for i in range(n)]


def file_offset(va):
    for name, vs, v, rs, ra in secs:
        if v <= va - base < v + rs:
            return va - base - v + ra
    raise ValueError(hex(va))


start = int(sys.argv[1], 16)
count = int(sys.argv[2]) if len(sys.argv) > 2 else 40
o = file_offset(start)
md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
for i, ins in enumerate(md.disasm(d[o:o + count * 16], start)):
    if i >= count:
        break
    print(f'{ins.address:#x}: {ins.bytes.hex():<20} {ins.mnemonic} {ins.op_str}')
