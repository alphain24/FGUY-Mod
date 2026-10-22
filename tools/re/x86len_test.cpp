// Research: checks src/loader/x86len.cpp against Capstone (tools/re/x86len_check.py builds and runs this).
//   x86len_test <section bytes file> <list file: "offset length mnemonic" per line>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
#include "../../src/loader/x86len.h"

int main(int argc, char** argv) {
    if (argc < 3) return 2;
    FILE* f = fopen(argv[1], "rb");
    if (!f) return 3;
    std::vector<unsigned char> code;
    unsigned char buf[65536];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) code.insert(code.end(), buf, buf + n);
    fclose(f);
    code.resize(code.size() + 32, 0xcc);
    FILE* l = fopen(argv[2], "r");
    if (!l) return 4;
    long total = 0, same = 0, unknown = 0, wrong = 0;
    std::vector<std::pair<unsigned, int>> all;
    unsigned off;
    int len;
    char mn[64];
    while (fscanf(l, "%x %d %63s", &off, &len, mn) == 3) {
        ++total;
        all.push_back({off, len});
        X86Insn ins;
        bool ok = X86Decode(&code[off], &ins);
        if (!ok) {
            if (++unknown <= 25) {
                printf("unknown  +%06x %-10s cap %d:", off, mn, len);
                for (int i = 0; i < len && i < 12; ++i) printf(" %02x", code[off + i]);
                printf("\n");
            }
        } else if (ins.length != len) {
            if (++wrong <= 40) {
                printf("WRONG    +%06x %-10s cap %d mine %d:", off, mn, len, ins.length);
                for (int i = 0; i < len && i < 12; ++i) printf(" %02x", code[off + i]);
                printf("\n");
            }
        } else {
            ++same;
        }
    }
    fclose(l);
    printf("%ld instructions: %ld the same, %ld wrong, %ld not known\n", total, same, wrong, unknown);
    // the start check: every instruction's start (yes), and one byte into each longer one (no)
    long yes = 0, no = 0, cant = 0, inYes = 0, inNo = 0, inCant = 0, shown = 0;
    for (auto& a : all) {
        const unsigned char* at = &code[a.first];
        int r = X86OnInstructionStart(at, &code[0], nullptr, nullptr);
        if (r == 1) ++yes;
        else if (r == 0) { ++no; if (++shown <= 10) printf("start judged NOT a start: +%06x\n", a.first); }
        else ++cant;
        if (a.second > 1) {
            int q = X86OnInstructionStart(at + 1, &code[0], nullptr, nullptr);
            if (q == 1) ++inYes; else if (q == 0) ++inNo; else ++inCant;
        }
    }
    printf("instruction starts: %ld yes, %ld no, %ld can't tell\n", yes, no, cant);
    printf("one byte in:        %ld yes (wrong), %ld no, %ld can't tell\n", inYes, inNo, inCant);
    return wrong ? 1 : 0;
}
