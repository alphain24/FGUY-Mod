"""Research: which instructions use a structure field, by its offset. Usage: fieldrefs.py <exe> <hex offset> [...]

Disassembles .text in one linear sweep (cached in build/text_disasm.pickle) and prints every instruction with a
[reg + offset] memory operand, writes first (the memory operand is the destination), with the function guess.
A linear sweep can misread data between functions; the function guess helps tell real hits.
"""
import os
import pickle
import sys

import capstone

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from pe import PE  # noqa: E402

CACHE = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..', 'build', 'text_disasm.pickle')


def sweep(exe):
    if os.path.exists(CACHE):
        return pickle.load(open(CACHE, 'rb'))
    name, va, vsize, rptr, rsize = exe.section('.text')
    md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
    md.detail = True
    code = exe.data[rptr:rptr + min(vsize, rsize)]
    out = []   # (address, text, displacement, is_write)
    off = 0
    while off < len(code):
        got = False
        for ins in md.disasm(code[off:off + 0x10000], va + off):
            got = True
            for i, op in enumerate(ins.operands):
                if op.type == capstone.x86.X86_OP_MEM and op.mem.base != 0 and op.mem.disp > 0x40:
                    write = i == 0 and ins.mnemonic not in ('cmp', 'test', 'push') and len(ins.operands) > 1
                    out.append((ins.address, f'{ins.mnemonic} {ins.op_str}', op.mem.disp, write))
            off = ins.address + ins.size - va
        if not got:
            off += 1
    pickle.dump(out, open(CACHE, 'wb'))
    return out


def main():
    exe = PE(sys.argv[1])
    refs = sweep(exe)
    for o in sys.argv[2:]:
        want = int(o, 16)
        hits = [r for r in refs if r[2] == want]
        hits.sort(key=lambda r: (not r[3], r[0]))
        print(f'##### +{want:#x}: {len(hits)} uses ({sum(1 for r in hits if r[3])} writes)')
        for a, text, _, w in hits:
            fs = exe.func_start(a)
            print(f'{"W" if w else " "} {a:08x}  {text:<50} (function {fs:08x})' if fs else f'{"W" if w else " "} {a:08x}  {text}')


if __name__ == '__main__':
    main()
