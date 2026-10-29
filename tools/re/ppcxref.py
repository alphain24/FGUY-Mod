"""Research: where Xbox 360 code builds an address (lis rX, hi + addi / a load or store off rX) - e.g. a string's.

Usage: python ppcxref.py <memory image> <base hex> <target hex> [<target hex> ...]
The image is the XEX's decrypted memory image (loaded at base). Prints each instruction that completes the address.
"""
import struct
import sys

d = open(sys.argv[1], "rb").read()
base = int(sys.argv[2], 16)
targets = {int(t, 16): t for t in sys.argv[3:]}
MEM = {14: "addi", 32: "lwz", 34: "lbz", 36: "stw", 38: "stb", 40: "lhz", 44: "sth", 48: "lfs", 50: "lfd", 52: "stfs"}
words = struct.unpack(">%dI" % (len(d) // 4), d[: len(d) // 4 * 4])
hi = {}   # register -> (value of its lis, where)
for i, w in enumerate(words):
    op = w >> 26
    rd, ra, imm = (w >> 21) & 31, (w >> 16) & 31, w & 0xFFFF
    if op == 15 and ra == 0:   # lis rd, imm
        hi[rd] = ((imm << 16) & 0xFFFFFFFF, i)
        continue
    if op in MEM and ra in hi and i - hi[ra][1] < 64:
        value = (hi[ra][0] + (imm - 0x10000 if imm & 0x8000 else imm)) & 0xFFFFFFFF
        if value in targets:
            print("%s: %#010x  %s (lis at %#010x)" % (targets[value], base + i * 4, MEM[op], base + hi[ra][1] * 4))
    if op == 18 or (op == 19 and ((w >> 1) & 0x3FF) in (16, 528)):   # (a branch: registers no longer known)
        if op == 19:
            hi.clear()
