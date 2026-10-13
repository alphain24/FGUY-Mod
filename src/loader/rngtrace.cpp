// The game's random numbers (research: what makes two PCs' matches drift apart).
//
// One Mersenne Twister for the whole game: state at 0xb09360 (624 words), the next word's address at 0xb09358,
// words left before the next refill at 0xada67c. Four functions hand out numbers, each starting by counting
// the words left down:
//   0x4f68e0 uint            dec [0xada67c]            (6 bytes)
//   0x4f6930 float 0..1      push ecx; dec [0xada67c]  (7)
//   0x4f69a0 float 0..<1     push ecx; dec [0xada67c]  (7)
//   0x4f6a20 a range         push ecx; fld [0x9f3360]  (7)
// Lockstep needs the game thread to get the same numbers on every PC, so: does any other thread take numbers
// from it (the render thread, drawing what's on this PC's screen)? Logged as it happens. And with
// FGUYMOD_RNGTRACE=1 every number the game thread takes in a session goes into FGUYMod\rng_<pid>.bin
// (tick, caller) - two PCs' files side by side show the first tick they differ (tools/rng_compare.py).
#include "common.h"

#include <cstdio>
#include <intrin.h>

DWORD NetplayGameThread();

namespace {

FILE* g_trace = nullptr;
CRITICAL_SECTION g_otherLock;
struct Other { DWORD thread, caller; unsigned count; };
Other g_others[32];
int g_otherCount = 0;

extern "C" void __cdecl RngRecord(DWORD caller, DWORD which) {
    DWORD me = GetCurrentThreadId();
    if (me == NetplayGameThread()) {
        if (g_trace && NetplayEnabled()) {
            static unsigned lastFlush = 0;
            DWORD entry[2] = {NetplayTick(), caller | (which << 28)};
            fwrite(entry, sizeof(entry), 1, g_trace);
            if (entry[0] >= lastFlush + 60) { lastFlush = entry[0]; fflush(g_trace); }   // (test copies get killed)
        }
        // FGUYMOD_RNGSTACK=<hex caller>: who gets there (the game addresses up the stack), the first 3 times
        static DWORD stackCaller = 0xffffffff;
        static int stacks = 0;
        if (stackCaller == 0xffffffff) {
            wchar_t v[32] = L"";
            SettingString(L"Debug", L"RngStack", L"FGUYMOD_RNGSTACK", L"", v, 32);
            stackCaller = v[0] ? wcstoul(v, nullptr, 16) : 0;
        }
        if (stackCaller && caller == stackCaller && stacks < 3) {
            ++stacks;
            const DWORD* sp = reinterpret_cast<const DWORD*>(_AddressOfReturnAddress());
            const DWORD* top = reinterpret_cast<const DWORD*>(reinterpret_cast<NT_TIB*>(NtCurrentTeb())->StackBase);
            wchar_t line[1024];
            int len = swprintf_s(line, L"Rng: tick %u, %#lx called from:", NetplayTick(), caller);
            for (int i = 0, found = 0; sp + i < top && i < 1500 && found < 30 && len < 980; ++i)
                if (sp[i] >= 0x401000 && sp[i] < 0x9f0000) { len += swprintf_s(line + len, 1024 - len, L" %#lx", sp[i]); ++found; }
            Log(L"%s", line);
        }
        return;
    }
    // Another thread: the first time for each (thread, caller) into the log, then every 1000th time.
    EnterCriticalSection(&g_otherLock);
    Other* o = nullptr;
    for (int i = 0; i < g_otherCount; ++i)
        if (g_others[i].thread == me && g_others[i].caller == caller) o = &g_others[i];
    if (!o && g_otherCount < 32) { o = &g_others[g_otherCount++]; *o = {me, caller, 0}; }
    unsigned n = o ? ++o->count : 0;
    LeaveCriticalSection(&g_otherLock);
    if (n == 1 || n % 1000 == 0)
        Log(L"Rng: thread %lu (not the game thread) takes a random number from %#lx (%u times so far)", me, caller, n);
}

// pushad (32) + pushfd (4) + the pushed `which` (4): the caller's return address is 40 bytes up.
__declspec(naked) void Stub0() {
    __asm {
        pushad
        pushfd
        push 0
        push dword ptr [esp + 40]
        call RngRecord
        add esp, 8
        popfd
        popad
        dec dword ptr ds:[0xada67c]
        push 0x4f68e6
        ret
    }
}
__declspec(naked) void Stub1() {
    __asm {
        pushad
        pushfd
        push 1
        push dword ptr [esp + 40]
        call RngRecord
        add esp, 8
        popfd
        popad
        push ecx
        dec dword ptr ds:[0xada67c]
        push 0x4f6937
        ret
    }
}
__declspec(naked) void Stub2() {
    __asm {
        pushad
        pushfd
        push 2
        push dword ptr [esp + 40]
        call RngRecord
        add esp, 8
        popfd
        popad
        push ecx
        dec dword ptr ds:[0xada67c]
        push 0x4f69a7
        ret
    }
}
__declspec(naked) void Stub3() {
    __asm {
        pushad
        pushfd
        push 3
        push dword ptr [esp + 40]
        call RngRecord
        add esp, 8
        popfd
        popad
        push ecx
        fld qword ptr ds:[0x9f3360]
        push 0x4f6a27
        ret
    }
}

bool Hook(uintptr_t at, const unsigned char* expected, size_t n, void* to) {
    auto b = reinterpret_cast<unsigned char*>(at);
    if (memcmp(b, expected, n) != 0) return false;
    DWORD old;
    VirtualProtect(b, n, PAGE_EXECUTE_READWRITE, &old);
    b[0] = 0xE9;
    *reinterpret_cast<int*>(b + 1) = (int)(reinterpret_cast<uintptr_t>(to) - (at + 5));
    for (size_t i = 5; i < n; ++i) b[i] = 0x90;
    VirtualProtect(b, n, old, &old);
    FlushInstructionCache(GetCurrentProcess(), b, n);
    return true;
}

}  // namespace

void InitRngTrace() {
    if (!SettingInt(L"Debug", L"RngWatch", L"FGUYMOD_RNGWATCH", 0) &&
        !SettingInt(L"Debug", L"RngTrace", L"FGUYMOD_RNGTRACE", 0))
        return;
    InitializeCriticalSection(&g_otherLock);
    static const unsigned char k0[] = {0xff, 0x0d, 0x7c, 0xa6, 0xad, 0x00};
    static const unsigned char k1[] = {0x51, 0xff, 0x0d, 0x7c, 0xa6, 0xad, 0x00};
    static const unsigned char k3[] = {0x51, 0xdd, 0x05, 0x60, 0x33, 0x9f, 0x00};
    int ok = Hook(0x4f68e0, k0, sizeof(k0), (void*)Stub0) + Hook(0x4f6930, k1, sizeof(k1), (void*)Stub1) +
             Hook(0x4f69a0, k1, sizeof(k1), (void*)Stub2) + Hook(0x4f6a20, k3, sizeof(k3), (void*)Stub3);
    if (SettingInt(L"Debug", L"RngTrace", L"FGUYMOD_RNGTRACE", 0)) {
        wchar_t path[MAX_PATH];
        swprintf_s(path, L"%s\\rng_%lu.bin", g_modDir, GetCurrentProcessId());
        if (_wfopen_s(&g_trace, path, L"wb") != 0) g_trace = nullptr;
        if (g_trace) setvbuf(g_trace, nullptr, _IOFBF, 1 << 20);
    }
    Log(L"Rng: watching the game's random numbers (%d of 4 hooks)%s", ok, g_trace ? L", game thread's into rng_<pid>.bin" : L"");
}

// Research: the players' cameras, every tick of a session, into FGUYMod\cam_<pid>.bin (FGUYMOD_CAMDUMP=1):
// tick, then per viewport 0..1 the camera's controller object (0x400 bytes) and the camera (0x400 bytes from
// its +0x100). Two PCs' files side by side: the first byte that differs (tools/cam_compare.py).
void CamDump(unsigned tick) {
    static int on = -1;
    static FILE* f = nullptr;
    if (on < 0) {
        on = SettingInt(L"Debug", L"CamDump", L"FGUYMOD_CAMDUMP", 0);
        if (on) {
            wchar_t path[MAX_PATH];
            swprintf_s(path, L"%s\\cam_%lu.bin", g_modDir, GetCurrentProcessId());
            if (_wfopen_s(&f, path, L"wb") != 0) f = nullptr;
        }
        Log(L"Rng: camera dump %s", !on ? L"off" : f ? L"on (cam_<pid>.bin)" : L"- can't open the file");
    }
    if (!f || tick > 3000) return;
    static unsigned char zero[0x400];
    fwrite(&tick, 4, 1, f);
    for (int v = 0; v < 2; ++v) {
        auto cam = reinterpret_cast<unsigned char*>(0xb5e2c0 + v * 0x3c70);
        auto ctl = *reinterpret_cast<unsigned char**>(cam + 0x1a8);
        fwrite(ctl && !IsBadReadPtr(ctl, 0x400) ? ctl : zero, 0x400, 1, f);
        fwrite(cam + 0x100, 0x400, 1, f);
    }
    if (tick % 60 == 0) fflush(f);
}
