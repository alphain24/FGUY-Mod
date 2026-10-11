"""Usage: dumpasm.py <exe> <out.asm> - the whole .text section as one listing (for grepping)."""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from pe import PE  # noqa: E402

e = PE(sys.argv[1])
name, va, vsize, rptr, rsize = next(s for s in e.sections if s[0] == '.text')
code = e.data[rptr:rptr + min(vsize, rsize)]
with open(sys.argv[2], 'w') as out:
    at = 0
    while at < len(code):
        last = at
        for ins in e.md.disasm(code[at:at + 0x10000], va + at):
            out.write(f'{ins.address:08x}  {ins.mnemonic} {ins.op_str}\n')
            last = ins.address - va + ins.size
        at = last if last > at else at + 1
