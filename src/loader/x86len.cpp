// x86 (32-bit) instruction lengths - see x86len.h. Checked against Capstone over all of FGUY.exe's code
// (tools/re/x86len_check.py).
#include "x86len.h"

namespace {

// The ModRM byte and what follows it (SIB, displacement): their bytes. a16: 16-bit addressing (0x67 prefix).
int ModRmBytes(const unsigned char* p, bool a16) {
    unsigned char modrm = p[0];
    int mod = modrm >> 6, rm = modrm & 7;
    if (mod == 3) return 1;
    if (a16) {
        if (mod == 0) return rm == 6 ? 3 : 1;
        return mod == 1 ? 2 : 3;
    }
    int n = 1;
    if (rm == 4) {   // SIB
        unsigned char sib = p[1];
        ++n;
        if (mod == 0 && (sib & 7) == 5) return n + 4;
    }
    if (mod == 0) return rm == 5 ? n + 4 : n;
    return mod == 1 ? n + 1 : n + 4;
}

}  // namespace

bool X86Decode(const unsigned char* p, X86Insn* out) {
    *out = X86Insn{};
    const unsigned char* start = p;
    bool o16 = false, a16 = false;
    for (int i = 0; i < 14; ++i, ++p) {   // prefixes
        unsigned char b = *p;
        if (b == 0x66) o16 = true;
        else if (b == 0x67) a16 = true;
        else if (b == 0xf0 || b == 0xf2 || b == 0xf3 || b == 0x26 || b == 0x2e || b == 0x36 || b == 0x3e || b == 0x64 ||
                 b == 0x65) {
        } else break;
    }
    const int immZ = o16 ? 2 : 4;   // (an operand-size immediate)
    unsigned char op = *p++;
    out->op = op;
    int modrm = 0, imm = 0, rel = 0;
    bool known = true;
    if (op == 0x0f) {
        unsigned char op2 = *p++;
        out->op2 = op2;
        if (op2 == 0x38) { ++p; modrm = 1; }                    // three-byte 0F 38 xx: ModRM
        else if (op2 == 0x3a) { ++p; modrm = 1; imm = 1; }      //   0F 3A xx: ModRM, imm8
        else if (op2 >= 0x80 && op2 <= 0x8f) rel = immZ;        // jcc rel32
        else if (op2 <= 0x03 || op2 == 0x0d || (op2 >= 0x10 && op2 <= 0x2f && !(op2 >= 0x24 && op2 <= 0x27)) ||
                 (op2 >= 0x40 && op2 <= 0x6f) || (op2 >= 0x74 && op2 <= 0x76) || (op2 >= 0x78 && op2 <= 0x7f) ||
                 (op2 >= 0x90 && op2 <= 0x9f) || op2 == 0xa3 || op2 == 0xa5 || op2 == 0xab || op2 == 0xad ||
                 op2 == 0xae || op2 == 0xaf || (op2 >= 0xb0 && op2 <= 0xb9) || (op2 >= 0xbb && op2 <= 0xc1) ||
                 op2 == 0xc3 || op2 == 0xc7 || op2 >= 0xd0)
            modrm = 1;
        else if ((op2 >= 0x70 && op2 <= 0x73) || op2 == 0xa4 || op2 == 0xac || op2 == 0xba || op2 == 0xc2 ||
                 (op2 >= 0xc4 && op2 <= 0xc6) || op2 == 0x0f) { modrm = 1; imm = 1; }
        else if ((op2 >= 0x05 && op2 <= 0x0b) || op2 == 0x0e || (op2 >= 0x30 && op2 <= 0x37) || op2 == 0x77 ||
                 op2 == 0xa0 || op2 == 0xa1 || op2 == 0xa2 || op2 == 0xa8 || op2 == 0xa9 || op2 == 0xaa ||
                 (op2 >= 0xc8 && op2 <= 0xcf)) {
        } else known = false;
    } else if (op < 0x40) {
        int lo = op & 7;
        if ((op & 0xe7) == 0x26 || op == 0x0f) known = false;   // (prefixes: taken above; 0x0f handled)
        else if (lo <= 3) modrm = 1;
        else if (lo == 4) imm = 1;
        else if (lo == 5) imm = immZ;
        // (06 07 0E 16 17 1E 1F 27 2F 37 3F: one byte)
    } else if (op < 0x60) {
    } else if (op == 0x60 || op == 0x61 || (op >= 0x6c && op <= 0x6f)) {
    } else if (op == 0x62 || op == 0x63) modrm = 1;
    else if (op == 0x68) imm = immZ;
    else if (op == 0x69) { modrm = 1; imm = immZ; }
    else if (op == 0x6a) imm = 1;
    else if (op == 0x6b) { modrm = 1; imm = 1; }
    else if (op >= 0x70 && op <= 0x7f) rel = 1;
    else if (op == 0x80 || op == 0x82 || op == 0x83) { modrm = 1; imm = 1; }
    else if (op == 0x81) { modrm = 1; imm = immZ; }
    else if (op >= 0x84 && op <= 0x8f) modrm = 1;
    else if ((op >= 0x90 && op <= 0x99) || (op >= 0x9b && op <= 0x9f)) {
    } else if (op == 0x9a || op == 0xea) imm = immZ + 2;   // far ptr16:32
    else if (op >= 0xa0 && op <= 0xa3) imm = a16 ? 2 : 4;   // moffs
    else if ((op >= 0xa4 && op <= 0xa7) || (op >= 0xaa && op <= 0xaf)) {
    } else if (op == 0xa8) imm = 1;
    else if (op == 0xa9) imm = immZ;
    else if (op >= 0xb0 && op <= 0xb7) imm = 1;
    else if (op >= 0xb8 && op <= 0xbf) imm = immZ;
    else if (op == 0xc0 || op == 0xc1 || op == 0xc6) { modrm = 1; imm = 1; }
    else if (op == 0xc2 || op == 0xca) { imm = 2; out->ends = op == 0xc2; }
    else if (op == 0xc3) out->ends = true;
    else if (op == 0xc4 || op == 0xc5) modrm = 1;
    else if (op == 0xc7) { modrm = 1; imm = immZ; }
    else if (op == 0xc8) imm = 3;
    else if (op == 0xc9 || op == 0xcb || op == 0xcc || op == 0xce || op == 0xcf) {
    } else if (op == 0xcd) imm = 1;
    else if (op >= 0xd0 && op <= 0xd3) modrm = 1;
    else if (op == 0xd4 || op == 0xd5) imm = 1;
    else if (op == 0xd6 || op == 0xd7) {
    } else if (op >= 0xd8 && op <= 0xdf) modrm = 1;   // x87
    else if (op >= 0xe0 && op <= 0xe3) rel = 1;       // loop / jcxz
    else if (op >= 0xe4 && op <= 0xe7) imm = 1;
    else if (op == 0xe8) rel = immZ;
    else if (op == 0xe9) { rel = immZ; out->ends = true; }
    else if (op == 0xeb) { rel = 1; out->ends = true; }
    else if ((op >= 0xec && op <= 0xef) || op == 0xf1 || op == 0xf4 || op == 0xf5 || (op >= 0xf8 && op <= 0xfd)) {
    } else if (op == 0xf6 || op == 0xf7) {
        modrm = 1;
        int reg = (p[0] >> 3) & 7;
        if (reg == 0 || reg == 1) imm = op == 0xf6 ? 1 : immZ;   // test r/m, imm
    } else if (op == 0xfe) modrm = 1;
    else if (op == 0xff) {
        modrm = 1;
        int reg = (p[0] >> 3) & 7;
        if (reg == 4 || reg == 5) out->ends = true;   // jmp r/m
    } else known = false;
    if (!known) return false;
    if (modrm) p += ModRmBytes(p, a16);
    if (rel) {
        out->relOffset = (int)(p - start);
        out->relSize = rel;
        p += rel;
    }
    p += imm;
    out->length = (int)(p - start);
    return out->length <= 15;
}

int X86OnInstructionStart(const unsigned char* at, const unsigned char* lowest, const unsigned char** before,
                          const unsigned char** after) {
    // the function's start: a 16-byte boundary right after int3 padding (two of them, or one after a ret) - how the
    // compiler lays functions out (99.6% of the game's)
    const unsigned char* f = reinterpret_cast<const unsigned char*>(reinterpret_cast<unsigned long>(at) & ~15ul);
    for (;; f -= 16) {
        if (f - 2 < lowest) return -1;
        if (f[-1] == 0xcc && (f[-2] == 0xcc || f[-2] == 0xc3) && f[0] != 0xcc) break;
    }
    const unsigned char* p = f;
    const unsigned char* prev = f;
    while (p < at) {
        X86Insn x;
        if (!X86Decode(p, &x)) return -1;
        prev = p;
        p += x.length;
    }
    if (p == at) return 1;
    if (before) *before = prev;
    if (after) *after = p;
    return 0;
}
