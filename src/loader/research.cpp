// Research aids (off unless asked for). Not needed to play.
//
// Player spy ([Debug] PlayerSpy=1 or env FGUYMOD_PLAYERSPY=1): wraps every method of the player
// class (zFamilyGuyPlayer, vtable 0xa1af14 in FGUY.exe) with a tiny stub that records `this` and
// counts calls, then jumps to the real method untouched. Every 10 seconds the log shows which
// methods run most (per-frame candidates) and each player's index and candidate position.

#include "common.h"

#include <cstdint>
#include <cstdio>
#include <cstring>

namespace {

const uintptr_t kPlayerVtable = 0xa1af14;
const int kPlayerSlots = 167;

LONG g_calls[kPlayerSlots];
void* volatile g_players[8];
bool g_spy = false;      // stubs installed
bool g_spyLog = false;   // periodic research log

void __cdecl Record(int slot, void* self) {
    InterlockedIncrement(&g_calls[slot]);
    for (int i = 0; i < 8; ++i) {
        if (g_players[i] == self) return;
        if (!g_players[i] &&
            InterlockedCompareExchangePointer(reinterpret_cast<void* volatile*>(&g_players[i]), self, nullptr) == nullptr)
            return;
    }
}

// pushad; pushfd; push ecx; push slot; call Record; add esp,8; popfd; popad; jmp original
void* MakeStub(int slot, void* original) {
    static unsigned char* page = nullptr;
    static size_t used = 0;
    const size_t size = 32;
    if (!page || used + size > 0x10000) {
        page = static_cast<unsigned char*>(VirtualAlloc(nullptr, 0x10000, MEM_COMMIT | MEM_RESERVE,
                                                        PAGE_EXECUTE_READWRITE));
        used = 0;
    }
    unsigned char* s = page + used;
    used += size;
    unsigned char* p = s;
    *p++ = 0x60;                                   // pushad
    *p++ = 0x9C;                                   // pushfd
    *p++ = 0x51;                                   // push ecx
    *p++ = 0x68; memcpy(p, &slot, 4); p += 4;      // push slot
    *p++ = 0xE8;                                   // call Record
    int32_t rel = (int32_t)((uintptr_t)&Record - ((uintptr_t)p + 4));
    memcpy(p, &rel, 4); p += 4;
    *p++ = 0x83; *p++ = 0xC4; *p++ = 0x08;         // add esp, 8
    *p++ = 0x9D;                                   // popfd
    *p++ = 0x61;                                   // popad
    *p++ = 0xE9;                                   // jmp original
    rel = (int32_t)((uintptr_t)original - ((uintptr_t)p + 4));
    memcpy(p, &rel, 4);
    return s;
}

bool Readable(const void* p, size_t n) { return p && !IsBadReadPtr(p, n); }

}  // namespace

void InitResearch() {
    g_spyLog = SettingInt(L"Debug", L"PlayerSpy", L"FGUYMOD_PLAYERSPY", 0) != 0;
    g_spy = g_spyLog || NetplayEnabled();  // netplay needs to know the players
    if (!g_spy) return;
    auto vt = reinterpret_cast<void**>(kPlayerVtable);
    DWORD old;
    VirtualProtect(vt, kPlayerSlots * sizeof(void*), PAGE_READWRITE, &old);
    for (int i = 0; i < kPlayerSlots; ++i) vt[i] = MakeStub(i, vt[i]);
    VirtualProtect(vt, kPlayerSlots * sizeof(void*), old, &old);
    Log(L"Research: player spy on (%d methods wrapped)", kPlayerSlots);
}

int ResearchPlayers(void** out, int max) {
    int n = 0;
    for (int i = 0; i < 8 && n < max; ++i)
        if (g_players[i]) out[n++] = g_players[i];
    return n;
}

void ResearchOnFrame(unsigned frame) {
    if (!g_spyLog || frame % 600) return;  // every ~10 s at 60 fps
    // Most-called methods since the last report.
    int top[6] = {-1, -1, -1, -1, -1, -1};
    LONG snap[kPlayerSlots];
    for (int i = 0; i < kPlayerSlots; ++i) snap[i] = InterlockedExchange(&g_calls[i], 0);
    for (int k = 0; k < 6; ++k)
        for (int i = 0; i < kPlayerSlots; ++i) {
            bool taken = false;
            for (int j = 0; j < k; ++j) taken |= top[j] == i;
            if (!taken && snap[i] && (top[k] < 0 || snap[i] > snap[top[k]])) top[k] = i;
        }
    wchar_t line[256] = L"";
    for (int k = 0; k < 6 && top[k] >= 0; ++k) {
        wchar_t one[32];
        swprintf_s(one, L" [%d]x%ld", top[k], snap[top[k]]);
        wcscat_s(line, one);
    }
    Log(L"Research: frame %u, busiest player methods:%s", frame, line);
    for (int i = 0; i < 8 && g_players[i]; ++i) {
        auto self = static_cast<unsigned char*>(g_players[i]);
        if (!Readable(self, 0xe00)) continue;
        int index = *reinterpret_cast<int*>(self + 0xdf8);
        auto xform = *reinterpret_cast<unsigned char**>(self + 0x38);
        float pos[3] = {0, 0, 0};
        if (Readable(xform, 0x40)) memcpy(pos, xform + 0x30, sizeof(pos));
        Log(L"Research:   player object %p index %d pos (%.2f, %.2f, %.2f)", self, index, pos[0], pos[1], pos[2]);
    }
}
