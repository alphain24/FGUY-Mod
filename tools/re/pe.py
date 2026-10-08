"""Tiny helpers for poking at FGUY.exe: address mapping, string xrefs, disassembly."""
import struct
import capstone


class PE:
    def __init__(self, path):
        self.data = open(path, 'rb').read()
        d = self.data
        pe = struct.unpack_from('<I', d, 0x3C)[0]
        nsec, optsz = struct.unpack_from('<H', d, pe + 6)[0], struct.unpack_from('<H', d, pe + 20)[0]
        opt = pe + 24
        self.imagebase = struct.unpack_from('<I', d, opt + 28)[0]
        self.sections = []
        for i in range(nsec):
            o = opt + optsz + 40 * i
            name = d[o:o + 8].rstrip(b'\0').decode()
            vsize, va, rsize, rptr = struct.unpack_from('<IIII', d, o + 8)
            self.sections.append((name, self.imagebase + va, vsize, rptr, rsize))
        self.md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)

    def section(self, name):
        return next(s for s in self.sections if s[0] == name)

    def off2va(self, off):
        for name, va, vs, rp, rs in self.sections:
            if rp <= off < rp + rs:
                return off - rp + va
        return None

    def va2off(self, va):
        for name, sva, vs, rp, rs in self.sections:
            if sva <= va < sva + max(vs, rs):
                return va - sva + rp
        return None

    def find_bytes(self, needle):
        out, i = [], self.data.find(needle)
        while i != -1:
            out.append(i)
            i = self.data.find(needle, i + 1)
        return out

    def string_va(self, s):
        """VAs of every NUL-terminated occurrence of an ASCII (or UTF-16 with wide=True) string."""
        b = s.encode() if isinstance(s, str) else s
        return [self.off2va(o) for o in self.find_bytes(b + b'\0') if o == 0 or self.data[o - 1] == 0]

    def xrefs(self, va):
        """Code locations in .text whose bytes contain this VA as a 32-bit immediate."""
        _, tva, _, trp, trs = self.section('.text')
        needle = struct.pack('<I', va)
        return [self.off2va(o) for o in self.find_bytes(needle) if trp <= o < trp + trs]

    def disasm(self, va, count=40, before=0):
        """Disassemble from va (optionally starting `before` bytes earlier, resynced)."""
        start = va - before
        off = self.va2off(start)
        code = self.data[off:off + count * 8 + before]
        lines = []
        for ins in self.md.disasm(code, start):
            mark = '>>' if ins.address == va else '  '
            lines.append(f'{mark} {ins.address:08x}  {ins.mnemonic:7} {ins.op_str}')
            if len(lines) >= count:
                break
        return '\n'.join(lines)

    def func_start(self, va, limit=0x4000):
        """Guess the start of the function containing va: walk back to padding (CC/90) before a prologue."""
        off = self.va2off(va)
        for o in range(off, off - limit, -1):
            if self.data[o - 1] in (0xCC, 0x90, 0xC3) and self.data[o:o + 3] in (b'\x55\x8b\xec', b'\x83\xec\x08') \
                    or (self.data[o - 1] in (0xCC,) and self.data[o] in (0x55, 0x83, 0x81, 0x53, 0x56, 0x57, 0x6a, 0x8b, 0xa1)):
                return self.off2va(o)
        return None

    def cstr_at(self, va, maxlen=200):
        off = self.va2off(va)
        if off is None:
            return None
        end = self.data.find(b'\0', off, off + maxlen)
        if end == -1:
            return None
        s = self.data[off:end]
        return s.decode('latin1') if all(32 <= c < 127 or c in (9, 10, 13) for c in s) and s else None
