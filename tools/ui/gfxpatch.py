"""Building blocks for patching Scaleform GFX movies (ActionScript 2) and writing .fgpatch files.

A .fgpatch file holds only our own bytes plus offsets, never the game's content, so it can be
shared publicly. The loader applies it in memory to the player's own copy of the movie.

.fgpatch format (little endian):
    "FGP1"  u32 original_length  u32 original_crc32  u32 edit_count
    edit_count x { u32 offset  u32 delete_length  u32 insert_length  bytes[insert_length] }
Edits are in original-file coordinates and must not overlap.
"""
import os
import struct
import sys
import zlib

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 're'))
import as2  # noqa: E402
import swf  # noqa: E402

# ---- tiny AS2 assembler ---------------------------------------------------------------------------

OP = {v: k for k, v in as2.OPS.items()}


def action(name, body=b''):
    code = OP[name]
    if code >= 0x80:
        return bytes([code]) + struct.pack('<H', len(body)) + body
    assert not body
    return bytes([code])


class Reg(int):
    """A register reference inside a Push."""


def push(*values):
    body = b''
    for v in values:
        if isinstance(v, Reg):
            body += b'\x04' + bytes([int(v)])
        elif isinstance(v, bool):
            body += b'\x05' + bytes([1 if v else 0])
        elif isinstance(v, int):
            body += b'\x07' + struct.pack('<i', v)
        elif isinstance(v, str):
            body += b'\x00' + v.encode('latin1') + b'\0'
        elif v is None:
            body += b'\x03'
        else:
            raise TypeError(v)
    return action('Push', body)


def external_call(method, *args):
    """flash.external.ExternalInterface.call(method, *args) -> leaves the result on the stack."""
    return (push(*reversed(args), method, len(args) + 1, 'flash') + action('GetVariable') +
            push('external') + action('GetMember') + push('ExternalInterface') + action('GetMember') +
            push('call') + action('CallMethod'))


def define_function(name, params, body):
    hdr = name.encode() + b'\0' + struct.pack('<H', len(params))
    for p in params:
        hdr += p.encode() + b'\0'
    hdr += struct.pack('<H', len(body))
    return action('DefineFunction', hdr) + body


def define_function2(name, params, body, registers=4):
    """DefineFunction2 with the given params in registers 1..n (no preloaded registers/flags)."""
    hdr = name.encode() + b'\0' + struct.pack('<HBH', len(params), max(registers, len(params) + 1), 0)
    for i, p in enumerate(params):
        hdr += bytes([i + 1]) + p.encode() + b'\0'
    hdr += struct.pack('<H', len(body))
    return action('DefineFunction2', hdr) + body


# ---- reading the original -------------------------------------------------------------------------

def push_records(data, o):
    """Records of the Push action at `o`: list of (offset, length, type, value)."""
    ln = struct.unpack_from('<H', data, o + 1)[0]
    q, end, out = o + 3, o + 3 + ln, []
    sizes = {1: 4, 2: 0, 3: 0, 4: 1, 5: 1, 6: 8, 7: 4, 8: 1, 9: 2}
    while q < end:
        t = data[q]
        if t == 0:
            e = data.index(b'\0', q + 1)
            out.append((q, e + 1 - q, t, data[q + 1:e].decode('latin1')))
            q = e + 1
            continue
        n = sizes[t]
        raw = data[q + 1:q + 1 + n]
        val = {7: lambda: struct.unpack('<i', raw)[0], 8: lambda: raw[0], 9: lambda: struct.unpack('<H', raw)[0],
               4: lambda: raw[0]}.get(t, lambda: raw)()
        out.append((q, 1 + n, t, val))
        q += 1 + n
    return out


def constant_pool(data, start):
    """The ConstantPool at the start of an action block (list of strings)."""
    assert data[start] == 0x88
    n = struct.unpack_from('<H', data, start + 3)[0]
    q, pool = start + 5, []
    for _ in range(n):
        e = data.index(b'\0', q)
        pool.append(data[q:e].decode('latin1'))
        q = e + 1
    return pool


def record_text(rec, pool):
    """String value of a push record (inline or constant-pool reference), else None."""
    off, ln, t, val = rec
    if t == 0:
        return val
    if t in (8, 9):
        return pool[val] if val < len(pool) else None
    return None


# ---- reading the original (actions) -----------------------------------------------------------------

def parse_actions(data, start, end):
    """Top-level action stream: list of (offset, code, total_length). Function bodies are skipped
    as part of their DefineFunction (total_length includes the body)."""
    out = []
    o = start
    while o < end:
        code = data[o]
        if code == 0:
            out.append((o, 0, 1))
            break
        ln = struct.unpack_from('<H', data, o + 1)[0] if code >= 0x80 else 0
        total = 1 + (2 + ln if code >= 0x80 else 0)
        body = o + 3
        if code == 0x9B:  # DefineFunction
            q = data.index(b'\0', body) + 1
            n = struct.unpack_from('<H', data, q)[0]; q += 2
            for _ in range(n):
                q = data.index(b'\0', q) + 1
            total += struct.unpack_from('<H', data, q)[0]
        elif code == 0x8E:  # DefineFunction2
            q = data.index(b'\0', body) + 1
            n = struct.unpack_from('<H', data, q)[0]; q += 2 + 1 + 2
            for _ in range(n):
                q = data.index(b'\0', q + 1) + 1
            total += struct.unpack_from('<H', data, q)[0]
        elif code == 0x94:  # With
            total += struct.unpack_from('<H', data, body)[0]
        out.append((o, code, total))
        o += total
    return out


def function_at(data, actions, name):
    """(offset, total_length) of the top-level DefineFunction/DefineFunction2 with this name."""
    for o, code, total in actions:
        if code in (0x9B, 0x8E) and data[o + 3:data.index(b'\0', o + 3)].decode('latin1') == name:
            return o, total
    raise KeyError(name)


# ---- edits ----------------------------------------------------------------------------------------

class Patch:
    def __init__(self, data):
        self.data = data
        self.edits = []  # (offset, delete, insert)

    def replace(self, offset, delete, insert):
        self.edits.append((offset, delete, bytes(insert)))

    def delta_between(self, a, b):
        """Net size change of edits fully inside [a, b)."""
        return sum(len(i) - d for o, d, i in self.edits if a <= o and o + d <= b)

    def fix_top_level_jumps(self, actions):
        """Re-aim Jump/If in this action list whose span crosses an edit. Adds overwrite edits.
        Works for any one level (top level or a function body)."""
        extra = []
        for o, code, total in actions:
            if code not in (0x99, 0x9D):
                continue
            off = struct.unpack_from('<h', self.data, o + 3)[0]
            src = o + total
            tgt = src + off
            lo, hi = (src, tgt) if tgt >= src else (tgt, src)
            delta = self.delta_between(lo, hi)
            if delta:
                new = off + delta if tgt >= src else off - delta
                extra.append((o + 3, 2, struct.pack('<h', new)))
        self.edits += extra

    def fix_length_u32(self, field_offset, region_start, region_end):
        """A u32 length field covering [region_start, region_end) in the original."""
        old = struct.unpack_from('<I', self.data, field_offset)[0]
        delta = self.delta_between(region_start, region_end)
        self.edits.append((field_offset, 4, struct.pack('<I', old + delta)))

    def apply(self):
        out, pos = bytearray(), 0
        for o, d, i in sorted(self.edits):
            assert o >= pos, 'overlapping edits'
            out += self.data[pos:o] + i
            pos = o + d
        return bytes(out + self.data[pos:])

    def to_fgpatch(self):
        blob = b'FGP1' + struct.pack('<III', len(self.data), zlib.crc32(self.data), len(self.edits))
        for o, d, i in sorted(self.edits):
            blob += struct.pack('<III', o, d, len(i)) + i
        return blob


def root_doaction_containing(data, offset):
    """(tag_offset, body_start, body_end) of the root-level DoAction tag containing `offset`."""
    for depth, code, body, ln, to in swf.walk(data, swf.header_end(data), len(data)):
        if depth == 0 and code == 12 and body <= offset < body + ln:
            return to, body, body + ln
    raise ValueError('no root DoAction there')
