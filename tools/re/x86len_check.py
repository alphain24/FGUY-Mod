"""Research: src/loader/x86len.cpp (the mods' Hook's instruction lengths) against Capstone, over FGUY.exe's code.

  x86len_check.py <FGUY.exe> <work dir>     builds tools/re/x86len_test.cpp (Visual Studio x86), compares every
                                            instruction of a linear sweep of .text
"""
import os
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from pe import PE  # noqa: E402

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, '..', '..'))
VS = r'C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvarsall.bat'

exe, work = sys.argv[1], sys.argv[2]
os.makedirs(work, exist_ok=True)
e = PE(exe)
name, va, vsize, rptr, rsize = next(s for s in e.sections if s[0] == '.text')
code = e.data[rptr:rptr + min(vsize, rsize)]
open(os.path.join(work, 'text.bin'), 'wb').write(code)
with open(os.path.join(work, 'list.txt'), 'w') as out:
    at = 0
    while at < len(code):
        last = at
        for ins in e.md.disasm(code[at:at + 0x10000], va + at):
            out.write(f'{ins.address - va:x} {ins.size} {ins.mnemonic.replace(" ", "_")}\n')
            last = ins.address - va + ins.size
        at = last if last > at else at + 1
test = os.path.join(work, 'x86len_test.exe')
src = os.path.join(HERE, 'x86len_test.cpp')
lib = os.path.join(ROOT, 'src', 'loader', 'x86len.cpp')
cmd = f'call "{VS}" x86 >nul && cl /nologo /O2 /EHsc /Fe"{test}" /Fo"{work}\\\\" "{src}" "{lib}"'
r = subprocess.run(cmd, shell=True, capture_output=True, text=True)
if r.returncode:
    print(r.stdout, r.stderr)
    sys.exit('build failed')
r = subprocess.run([test, os.path.join(work, 'text.bin'), os.path.join(work, 'list.txt')], capture_output=True, text=True)
print(r.stdout)
sys.exit(r.returncode)
