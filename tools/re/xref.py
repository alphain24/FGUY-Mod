"""Usage: xref.py <exe> <string> [<string> ...]
Prints every code location that references each string, with disassembly and the strings
referenced nearby (push/mov of string addresses), which usually reveals what the code does."""
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from pe import PE  # noqa: E402

exe = PE(sys.argv[1])
for s in sys.argv[2:]:
    for sva in exe.string_va(s):
        refs = exe.xrefs(sva)
        print(f'\n##### "{s}" @ {sva:08x}  ({len(refs)} code refs)')
        for r in refs:
            fs = exe.func_start(r)
            print(f'\n--- ref at {r:08x} (function guess {fs:08x})' if fs else f'\n--- ref at {r:08x}')
            text = exe.disasm(r - 1, count=30, before=48)
            for line in text.splitlines():
                strs = [exe.cstr_at(int(h, 16)) for h in re.findall(r'0x[0-9a-f]{6,8}', line)]
                strs = [x for x in strs if x]
                print(line + (f'    ; "{strs[0][:60]}"' if strs else ''))
