// Research aids (off unless asked for). Not needed to play.
//
// Player spy ([Debug] PlayerSpy=1 or env FGUYMOD_PLAYERSPY=1): wraps every method of the player
// class (zFamilyGuyPlayer, vtable 0xa1af14 in FGUY.exe) with a tiny stub that records `this` and
// counts calls, then jumps to the real method untouched. Every 10 seconds the log shows which
// methods run most (per-frame candidates) and each player's index and candidate position.

#include "common.h"

#include <intrin.h>

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

volatile DWORD g_callerTids[6];
volatile LONG g_callerCounts[6];

void __cdecl Record(int slot, void* self) {
    InterlockedIncrement(&g_calls[slot]);
    DWORD me = GetCurrentThreadId();
    for (int i = 0; i < 6; ++i) {
        if (g_callerTids[i] == me) { InterlockedIncrement(&g_callerCounts[i]); break; }
        if (!g_callerTids[i] && InterlockedCompareExchange(&g_callerTids[i], me, 0) == 0) { g_callerCounts[i] = 1; break; }
    }
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

// Thread creation log: which engine thread is which (CreateThread and the C runtime's _beginthreadex).
decltype(&CreateThread) Real_CreateThread;
using beginthreadex_t = uintptr_t(__cdecl*)(void*, unsigned, unsigned(__stdcall*)(void*), void*, unsigned, unsigned*);
beginthreadex_t Real_beginthreadex;
uintptr_t __cdecl Hook_beginthreadex(void* sa, unsigned stack, unsigned(__stdcall* start)(void*), void* arg, unsigned flags,
                                     unsigned* tidOut) {
    unsigned tid = 0;
    uintptr_t h = Real_beginthreadex(sa, stack, start, arg, flags, &tid);
    if (tidOut) *tidOut = tid;
    Log(L"Research: thread %u created (_beginthreadex), start %p, arg %p (by thread %lu)", tid, (void*)start, arg,
        GetCurrentThreadId());
    return h;
}
HANDLE WINAPI Hook_CreateThread(LPSECURITY_ATTRIBUTES sa, SIZE_T stack, LPTHREAD_START_ROUTINE start, LPVOID arg,
                                DWORD flags, LPDWORD tidOut) {
    DWORD tid = 0;
    HANDLE h = Real_CreateThread(sa, stack, start, arg, flags, &tid);
    if (tidOut) *tidOut = tid;
    Log(L"Research: thread %lu created, start %p, arg %p (by thread %lu)", tid, (void*)start, arg, GetCurrentThreadId());
    return h;
}

}  // namespace

// Damage spy ([Debug] DamageSpy=1 or env FGUYMOD_DAMAGESPY=1): logs every call of the players' TakeDamage
// (vtable slot 58, 0x6901a0) - victim, source, amount, type, flags, direction, health before and after.
using TakeDamage_t = char(__thiscall*)(void* self, void* info);
TakeDamage_t Real_TakeDamage;

int PlayerIndexOf(const void* entity) {
    if (!entity || IsBadReadPtr(entity, 0xe00)) return -99;
    if (*reinterpret_cast<const uintptr_t*>(entity) != kPlayerVtable) return -1;
    return *reinterpret_cast<const int*>(static_cast<const char*>(entity) + 0xdf8);
}

float HealthOf(const void* player) {
    auto h = *reinterpret_cast<const char* const*>(static_cast<const char*>(player) + 0x105c);
    return h ? *reinterpret_cast<const float*>(h + 0x50) : -1.f;
}

char __fastcall Spy_TakeDamage(void* self, void*, void* info) {
    const char* d = static_cast<const char*>(info);
    const void* src = *reinterpret_cast<void* const*>(d);
    float before = HealthOf(self);
    char r = Real_TakeDamage(self, info);
    const float* f = reinterpret_cast<const float*>(d);
    const int* n = reinterpret_cast<const int*>(d);
    unsigned srcVt = src && !IsBadReadPtr(src, 0x24) ? *reinterpret_cast<const unsigned*>(src) : 0;
    int srcType = src && !IsBadReadPtr(src, 0x24) ? *reinterpret_cast<const int*>(static_cast<const char*>(src) + 0x20) : 0;
    Log(L"DamageSpy: player %d hit by %p (vtable %#x, type %#x, player %d): amount %.2f type %d flags %#x "
        L"dir (%.2f %.2f %.2f) +4 %d +8 %.2f +0x60 %d +0x64 %.2f; health %.1f -> %.1f, returned %d",
        PlayerIndexOf(self), src, srcVt, srcType, PlayerIndexOf(src), f[7], n[8], n[10], f[12], f[13], f[14],
        n[1], f[2], n[0x18], f[0x19], before, HealthOf(self), r);
    wchar_t raw[600];
    int len = 0;
    for (int i = 0; i < 0x30 && len < 560; ++i) len += swprintf_s(raw + len, 600 - len, L" %08x", (unsigned)n[i]);
    Log(L"DamageSpy:   called from %p, raw%s", _ReturnAddress(), raw);
    return r;
}

void InitResearch() {
    if (SettingInt(L"Debug", L"DamageSpy", L"FGUYMOD_DAMAGESPY", 0)) {
        auto slot = reinterpret_cast<void**>(kPlayerVtable) + 58;
        DWORD old;
        VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &old);
        Real_TakeDamage = reinterpret_cast<TakeDamage_t>(*slot);
        *slot = reinterpret_cast<void*>(&Spy_TakeDamage);
        VirtualProtect(slot, sizeof(void*), old, &old);
        Log(L"Research: damage spy on (TakeDamage %p)", Real_TakeDamage);
    }
    if (SettingInt(L"Debug", L"ThreadLog", L"FGUYMOD_THREADLOG", 0)) {
        HookSpec hooks[] = {{"kernel32.dll", "CreateThread", (void*)Hook_CreateThread, (void**)&Real_CreateThread},
                            {"msvcr100.dll", "_beginthreadex", (void*)Hook_beginthreadex, (void**)&Real_beginthreadex}};
        InstallHooks(GetModuleHandleW(nullptr), hooks);
    }
    g_spyLog = SettingInt(L"Debug", L"PlayerSpy", L"FGUYMOD_PLAYERSPY", 0) != 0;
    g_spy = g_spyLog;
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
    Log(L"Research:   player methods called by thread %lu x%ld, %lu x%ld, %lu x%ld, %lu x%ld (render thread %lu)",
        g_callerTids[0], g_callerCounts[0], g_callerTids[1], g_callerCounts[1], g_callerTids[2], g_callerCounts[2],
        g_callerTids[3], g_callerCounts[3], GetCurrentThreadId());
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
