// Research: which game code writes a value? Hardware watchpoints (the CPU's debug registers, on every thread of
// the game; up to four addresses) on 4 bytes each; each place that writes them goes into the log once, with its
// callers and the netplay tick, and how often it writes. Test pipe: "watch <hex address>", "watchaim <player>".
#include "common.h"

#include <tlhelp32.h>

namespace {

struct Writer { int slot; DWORD eip; unsigned count; };
Writer g_writers[64];
volatile LONG g_writerCount = 0;
volatile LONG g_hits = 0;
bool g_handler = false;
DWORD g_addr[4] = {};
int g_slots = 0;

LONG CALLBACK WatchHandler(EXCEPTION_POINTERS* e) {
    if (e->ExceptionRecord->ExceptionCode != EXCEPTION_SINGLE_STEP || !(e->ContextRecord->Dr6 & 0xf))
        return EXCEPTION_CONTINUE_SEARCH;
    CONTEXT* c = e->ContextRecord;
    int slot = 0;
    while (slot < 3 && !(c->Dr6 & (1u << slot))) ++slot;
    c->Dr6 = 0;
    LONG hits = InterlockedIncrement(&g_hits);
    Writer* w = nullptr;
    for (LONG i = 0; i < g_writerCount; ++i)
        if (g_writers[i].eip == c->Eip && g_writers[i].slot == slot) w = &g_writers[i];
    static DWORD last[4] = {0xfeedf00d, 0xfeedf00d, 0xfeedf00d, 0xfeedf00d};
    DWORD now = *reinterpret_cast<DWORD*>(g_addr[slot]);
    bool changed = now != last[slot];
    last[slot] = now;
    static volatile LONG changes = 0;
    if (w) {
        ++w->count;
        if (changed && InterlockedIncrement(&changes) <= 60) {   // a new value: when (tick), and from where
            wchar_t line[1024];
            int len = swprintf_s(line, L"Watch: %#lx = %#lx at tick %u (code before %#lx) - callers:", g_addr[slot], now,
                                 NetplayTick(), c->Eip);
            const DWORD* sp = reinterpret_cast<const DWORD*>(c->Esp);
            for (int i = 0, found = 0; i < 600 && found < 20 && len < 960; ++i) {
                if (IsBadReadPtr(sp + i, 4)) break;
                if (sp[i] >= 0x401000 && sp[i] < 0x9f0000) { len += swprintf_s(line + len, 1024 - len, L" %#lx", sp[i]); ++found; }
            }
            Log(L"%s", line);
        }
    } else if (g_writerCount < 64) {
        w = &g_writers[g_writerCount];
        *w = {slot, c->Eip, 1};
        InterlockedIncrement(&g_writerCount);
        wchar_t line[1024];
        int len = swprintf_s(line, L"Watch: %#lx (now %#lx) written at tick %u by code before %#lx (thread %lu) - callers:",
                             g_addr[slot], *reinterpret_cast<DWORD*>(g_addr[slot]), NetplayTick(), c->Eip,
                             GetCurrentThreadId());
        const DWORD* sp = reinterpret_cast<const DWORD*>(c->Esp);
        for (int i = 0, found = 0; i < 400 && found < 14 && len < 960; ++i) {
            if (IsBadReadPtr(sp + i, 4)) break;
            if (sp[i] >= 0x401000 && sp[i] < 0x9f0000) { len += swprintf_s(line + len, 1024 - len, L" %#lx", sp[i]); ++found; }
        }
        Log(L"%s", line);
    } else if (w == nullptr && hits % 1000 == 0) {
        // (all writer slots used)
    }
    if (hits == 20000) {   // enough: the counts, and the watchpoints off on this thread
        for (LONG i = 0; i < g_writerCount; ++i)
            Log(L"Watch: %#lx: %#lx wrote %u times", g_addr[g_writers[i].slot], g_writers[i].eip, g_writers[i].count);
        c->Dr7 &= ~0xffu;
    }
    return EXCEPTION_CONTINUE_EXECUTION;
}

}  // namespace

void WatchAddress(const void* addr, const wchar_t* what, bool reads) {
    if (!g_handler) { AddVectoredExceptionHandler(1, WatchHandler); g_handler = true; }
    if (g_slots >= 4) { Log(L"Watch: all four watchpoints are in use"); return; }
    int slot = g_slots++;
    g_addr[slot] = reinterpret_cast<DWORD>(addr);
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    THREADENTRY32 te{sizeof(te)};
    int set = 0;
    for (BOOL ok = Thread32First(snap, &te); ok; ok = Thread32Next(snap, &te)) {
        if (te.th32OwnerProcessID != GetCurrentProcessId() || te.th32ThreadID == GetCurrentThreadId()) continue;
        HANDLE h = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_SET_CONTEXT, FALSE, te.th32ThreadID);
        if (!h) continue;
        if (SuspendThread(h) != (DWORD)-1) {
            CONTEXT c{};
            c.ContextFlags = CONTEXT_DEBUG_REGISTERS;
            if (GetThreadContext(h, &c)) {
                (&c.Dr0)[slot] = g_addr[slot];
                // this slot on, on writes (01) or reads and writes (11), 4 bytes (11)
                c.Dr7 = (c.Dr7 & ~((3u << (slot * 2)) | (0xfu << (16 + slot * 4)))) | (1u << (slot * 2)) |
                        ((reads ? 0xfu : 0xdu) << (16 + slot * 4));
                if (SetThreadContext(h, &c)) ++set;
            }
            ResumeThread(h);
        }
        CloseHandle(h);
    }
    CloseHandle(snap);
    Log(L"Watch: %s at %p (watchpoint %d%s) - on %d threads", what, addr, slot, reads ? L", reads too" : L"", set);
}
