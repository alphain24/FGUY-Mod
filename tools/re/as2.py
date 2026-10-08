"""ActionScript 2 bytecode disassembler for DoAction / DoInitAction / button actions.

Usage: as2.py <file.gfx> [--find TEXT]   (prints every action block, or only blocks mentioning TEXT)
"""
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import swf  # noqa: E402

OPS = {0x04: 'NextFrame', 0x05: 'PrevFrame', 0x06: 'Play', 0x07: 'Stop', 0x0A: 'Add', 0x0B: 'Subtract',
       0x0C: 'Multiply', 0x0D: 'Divide', 0x0E: 'Equals', 0x0F: 'Less', 0x10: 'And', 0x11: 'Or', 0x12: 'Not',
       0x13: 'StringEquals', 0x14: 'StringLength', 0x15: 'StringExtract', 0x17: 'Pop', 0x18: 'ToInteger',
       0x1C: 'GetVariable', 0x1D: 'SetVariable', 0x20: 'SetTarget2', 0x21: 'StringAdd', 0x22: 'GetProperty',
       0x23: 'SetProperty', 0x24: 'CloneSprite', 0x25: 'RemoveSprite', 0x26: 'Trace', 0x27: 'StartDrag',
       0x28: 'EndDrag', 0x29: 'StringLess', 0x2A: 'Throw', 0x2B: 'CastOp', 0x2C: 'ImplementsOp',
       0x30: 'RandomNumber', 0x34: 'GetTime', 0x3A: 'Delete', 0x3B: 'Delete2', 0x3C: 'DefineLocal',
       0x3D: 'CallFunction', 0x3E: 'Return', 0x3F: 'Modulo', 0x40: 'NewObject', 0x41: 'DefineLocal2',
       0x42: 'InitArray', 0x43: 'InitObject', 0x44: 'TypeOf', 0x45: 'TargetPath', 0x46: 'Enumerate',
       0x47: 'Add2', 0x48: 'Less2', 0x49: 'Equals2', 0x4A: 'ToNumber', 0x4B: 'ToString',
       0x4C: 'PushDuplicate', 0x4D: 'StackSwap', 0x4E: 'GetMember', 0x4F: 'SetMember', 0x50: 'Increment',
       0x51: 'Decrement', 0x52: 'CallMethod', 0x53: 'NewMethod', 0x54: 'InstanceOf', 0x55: 'Enumerate2',
       0x60: 'BitAnd', 0x61: 'BitOr', 0x62: 'BitXor', 0x63: 'BitLShift', 0x64: 'BitRShift',
       0x65: 'BitURShift', 0x66: 'StrictEquals', 0x67: 'Greater', 0x68: 'StringGreater', 0x69: 'Extends',
       0x81: 'GotoFrame', 0x83: 'GetURL', 0x87: 'StoreRegister', 0x88: 'ConstantPool', 0x8A: 'WaitForFrame',
       0x8B: 'SetTarget', 0x8C: 'GoToLabel', 0x8D: 'WaitForFrame2', 0x8E: 'DefineFunction2', 0x8F: 'Try',
       0x94: 'With', 0x96: 'Push', 0x99: 'Jump', 0x9A: 'GetURL2', 0x9B: 'DefineFunction', 0x9D: 'If',
       0x9E: 'Call', 0x9F: 'GotoFrame2'}


def cstr(b, o):
    e = b.index(b'\0', o)
    return b[o:e].decode('latin1'), e + 1


def disasm(data, start, end, pool=None, indent=''):
    """Returns list of text lines. `pool` is the current constant pool (list)."""
    pool = pool if pool is not None else []
    lines = []
    o = start
    while o < end:
        at = o
        code = data[o]
        o += 1
        if code == 0:
            lines.append(f'{indent}{at:06x}: End')
            break
        ln = 0
        if code >= 0x80:
            ln = struct.unpack_from('<H', data, o)[0]
            o += 2
        body = data[o:o + ln]
        name = OPS.get(code, f'op{code:#x}')
        arg = ''
        if code == 0x88:
            n = struct.unpack_from('<H', body, 0)[0]
            p, q = [], 2
            for _ in range(n):
                s, q = cstr(body, q)
                p.append(s)
            pool[:] = p
            arg = f'{n} strings'
        elif code == 0x96:
            vals, q = [], 0
            while q < ln:
                t = body[q]; q += 1
                if t == 0: s, q = cstr(body, q); vals.append(repr(s))
                elif t == 1: vals.append(f'{struct.unpack_from("<f", body, q)[0]:g}'); q += 4
                elif t == 2: vals.append('null')
                elif t == 3: vals.append('undefined')
                elif t == 4: vals.append(f'r{body[q]}'); q += 1
                elif t == 5: vals.append('true' if body[q] else 'false'); q += 1
                elif t == 6: vals.append(f'{struct.unpack_from("<d", body[q+4:q+8] + body[q:q+4])[0]:g}'); q += 8
                elif t == 7: vals.append(str(struct.unpack_from('<i', body, q)[0])); q += 4
                elif t == 8: i = body[q]; q += 1; vals.append(repr(pool[i]) if i < len(pool) else f'c{i}')
                elif t == 9: i = struct.unpack_from('<H', body, q)[0]; q += 2; vals.append(repr(pool[i]) if i < len(pool) else f'c{i}')
                else: vals.append(f'?type{t}'); break
            arg = ', '.join(vals)
        elif code in (0x99, 0x9D):
            off = struct.unpack_from('<h', body, 0)[0]
            arg = f'-> {o + ln + off:06x}'
        elif code == 0x87:
            arg = f'r{body[0]}'
        elif code in (0x8C,):
            arg = repr(cstr(body, 0)[0])
        elif code == 0x81:
            arg = str(struct.unpack_from('<H', body, 0)[0])
        elif code == 0x9B:
            fname, q = cstr(body, 0)
            n = struct.unpack_from('<H', body, q)[0]; q += 2
            params = []
            for _ in range(n):
                s, q = cstr(body, q); params.append(s)
            size = struct.unpack_from('<H', body, q)[0]
            lines.append(f'{indent}{at:06x}: function {fname or "(anon)"}({", ".join(params)})  {{')
            lines += disasm(data, o + ln, o + ln + size, pool, indent + '    ')
            lines.append(f'{indent}}}')
            o += ln + size
            continue
        elif code == 0x8E:
            fname, q = cstr(body, 0)
            n = struct.unpack_from('<H', body, q)[0]; q += 2
            nregs = body[q]; q += 1
            q += 2  # flags
            params = []
            for _ in range(n):
                reg = body[q]; q += 1
                s, q = cstr(body, q)
                params.append(f'{s}@r{reg}' if reg else s)
            size = struct.unpack_from('<H', body, q)[0]
            lines.append(f'{indent}{at:06x}: function2 {fname or "(anon)"}({", ".join(params)}) regs={nregs}  {{')
            lines += disasm(data, o + ln, o + ln + size, pool, indent + '    ')
            lines.append(f'{indent}}}')
            o += ln + size
            continue
        elif code == 0x94:
            size = struct.unpack_from('<H', body, 0)[0]
            lines.append(f'{indent}{at:06x}: with {{')
            lines += disasm(data, o + ln, o + ln + size, pool, indent + '    ')
            lines.append(f'{indent}}}')
            o += ln + size
            continue
        lines.append(f'{indent}{at:06x}: {name} {arg}'.rstrip())
        o += ln
    return lines


def action_blocks(data):
    """(label, start, end) for each DoAction / DoInitAction body in the file."""
    out = []
    sprite = None
    for depth, code, body, ln, to in swf.walk(data, swf.header_end(data), len(data)):
        if code == 39:
            sprite = struct.unpack_from('<H', data, body)[0]
        if code == 12:
            out.append((f'DoAction (sprite {sprite if depth else "root"}) @ {to:#x}', body, body + ln))
        elif code == 59:
            sid = struct.unpack_from('<H', data, body)[0]
            out.append((f'DoInitAction (sprite {sid}) @ {to:#x}', body + 2, body + ln))
    return out


if __name__ == '__main__':
    data = open(sys.argv[1], 'rb').read()
    find = sys.argv[sys.argv.index('--find') + 1] if '--find' in sys.argv else None
    for label, s, e in action_blocks(data):
        if find and find.encode() not in data[s:e]:
            continue
        print(f'===== {label}  ({e - s} bytes)')
        print('\n'.join(disasm(data, s, e)))
