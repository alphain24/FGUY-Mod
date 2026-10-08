"""Find C++ vtables from MSVC RTTI. Usage: rtti.py <exe> <class-substring> [...]

For each matching class prints the vtable address(es) and the first methods' addresses.
MSVC layout: TypeDescriptor { vftable*, spare, char name[] } ; CompleteObjectLocator
{ sig, offset, cdOffset, TypeDescriptor*, ClassHierarchyDescriptor* } ; vtable[-1] = &COL.
"""
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from pe import PE  # noqa: E402


def find_vtables(exe, name_substr, nmethods=24):
    results = []
    d = exe.data
    for off in exe.find_bytes(b'.?AV') + exe.find_bytes(b'.?AU'):
        end = d.index(b'\0', off)
        name = d[off:end].decode('latin1')
        if name_substr not in name:
            continue
        td = exe.off2va(off - 8)
        # COLs: sig 0, then offset, cdOffset, pTypeDescriptor == td
        for col_ptr_off in exe.find_bytes(struct.pack('<I', td)):
            col = exe.off2va(col_ptr_off - 12)
            if col is None:
                continue
            sig, offset, cd = struct.unpack_from('<III', d, col_ptr_off - 12)
            if sig != 0:
                continue
            for vt_ref in exe.find_bytes(struct.pack('<I', col)):
                vt = exe.off2va(vt_ref) + 4
                methods = []
                for i in range(nmethods):
                    o = exe.va2off(vt + 4 * i)
                    if o is None:
                        break
                    m = struct.unpack_from('<I', d, o)[0]
                    if not (0x401000 <= m < 0x9dc000):
                        break
                    methods.append(m)
                results.append((name, offset, vt, methods))
    return results


if __name__ == '__main__':
    exe = PE(sys.argv[1])
    for sub in sys.argv[2:]:
        for name, offset, vt, methods in find_vtables(exe, sub):
            print(f'{name}  (subobject offset {offset})  vtable {vt:#x}')
            for i, m in enumerate(methods):
                print(f'    [{i:2}] +{4*i:#04x}  {m:#x}')
