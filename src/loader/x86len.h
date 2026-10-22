// A small x86 (32-bit) instruction length decoder, for the mods' Hook (modloader.cpp): where instructions start and
// end, and the relative branch / call in one (to move it into a trampoline). Covers the general-purpose, x87, MMX and
// SSE instructions a 32-bit game uses; anything else: 0 (not known - the caller refuses rather than guessing).
#pragma once

struct X86Insn {
    int length;      // its bytes (0: not decoded)
    int relOffset;   // a relative branch / call: where its displacement starts (else 0)
    int relSize;     //   and its size: 1 or 4 (2 with an operand-size prefix); 0 if none
    unsigned char op;   // the opcode byte (after prefixes); 0x0f: a two-byte one, op2 its second byte
    unsigned char op2;
    bool ends;       // nothing after it runs next: ret, an unconditional jmp
};

bool X86Decode(const unsigned char* p, X86Insn* out);

// Does an instruction start at `at`? Decoded forward from the start of the function around it (back to the int3
// padding before it, on a 16-byte boundary - not below `lowest`): 1 yes; 0 no - *before / *after the instruction
// starts around it; -1 can't tell (no function start found, or code this decoder doesn't know on the way).
int X86OnInstructionStart(const unsigned char* at, const unsigned char* lowest, const unsigned char** before,
                          const unsigned char** after);
