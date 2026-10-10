// Netplay core: lockstep input sync.
//
// Every peer runs the same game. Each peer owns one player slot (= one virtual XInput pad). Every game
// tick, each peer sends its pad state for (tick + InputDelay) to the others and waits until it has every
// slot's state for the current tick, so all peers feed identical inputs into identical ticks.
//
// How the game runs a tick (measured, see RESEARCH_NOTES.md): the MAIN thread (the one that loads us)
// runs the game logic. Each tick it waits for the render thread's go-ahead, then reads all controllers
// itself (XInputGetState for pads 0..18), updates the game (handing jobs to worker threads), and hands
// the frame to the render thread, which draws and presents it while the main thread starts the next tick.
// So the lockstep step happens on the main thread right at the start of a tick: at its first controller
// poll. Everything a tick reads is then decided before the tick runs.
//
// For the ticks to stay identical everywhere:
//   * Lockstep starts at the very first tick (all peers meet there), so menus are lockstepped too.
//   * The main thread sees a fixed clock: exactly base + tick x 1/60 s at the start of every tick, the same
//     on every PC (QPC, timeGetTime, GetTickCount).
//   * The C runtime random generator gets a fixed seed.
//   * Background work finishes inside the tick that started it: file reads complete before ReadFile
//     returns, and each tick starts only when the game's other threads (except the render thread, which is
//     busy drawing the previous frame by design) are parked in a wait/sleep ("quiescence").
//   * Local keyboard / mouse / cursor don't reach the game (dinput.cpp, Hook_GetCursorPos).
// Every SyncCheckFrames ticks peers compare a hash of the players' positions, taken at the tick boundary.
// Scenes that hold the game thread (loading screens) are skipped for everyone when any player skips.
//
// Settings ([Netplay] in FGUYMod.ini, env overrides for tests):
//   Enable=1  Slot=0..3  Players=2  Port=7600  Peers=127.0.0.1:7601,...  InputDelay=3
//   Script=1 (scripted test inputs instead of the real pad)  SyncCheckFrames=60
// Research knobs (env only): FGUYMOD_NET_IDLE=1 (script only leaves the start pause),
//   FGUYMOD_NET_TRACE=<tick> (timeline of thread handoffs for 4 ticks).

#include "common.h"

#include <winsock2.h>
#include <ws2tcpip.h>
#include <xinput.h>

#include <intrin.h>

#include <cstdio>
#include <cstring>
#include <vector>

#pragma comment(lib, "ws2_32.lib")

namespace {

// ---- settings ---------------------------------------------------------------------------------------
bool g_enabled = false;
int g_slot = 0, g_players = 2, g_delay = 3, g_syncEvery = 60;
bool g_script = false, g_idleScript = false;
unsigned g_traceFrom = 0;
std::vector<sockaddr_in> g_peers;

// ---- state ------------------------------------------------------------------------------------------
struct Pad {
    WORD buttons;
    BYTE lt, rt;
    SHORT lx, ly, rx, ry;
};
const int kRing = 256;               // ticks of input history kept
Pad g_inputs[kRing][4];
bool g_have[kRing][4];
unsigned g_haveTick[kRing][4];       // which tick each ring entry holds
Pad g_current[4];                    // what XInputGetState reports during this tick

enum class Phase { Off, Boot, Running };
Phase g_phase = Phase::Off;
unsigned g_tick = 0;                 // lockstep tick being simulated
bool g_peerReady[4];
DWORD g_mainThread = 0;              // the thread that loaded us = the game logic thread
volatile DWORD g_renderThread = 0;   // the thread that calls Present
SOCKET g_sock = INVALID_SOCKET;
LARGE_INTEGER g_freq;
decltype(&QueryPerformanceCounter) Real_QPC;

// ---- research: timeline of thread handoffs -----------------------------------------------------------
struct TraceEvent { LONGLONG t; DWORD thread; int what; ULONG_PTR arg; };
TraceEvent g_trace[8192];
volatile LONG g_traceCount = 0;
volatile bool g_traceOn = false;
void Trace(int what, ULONG_PTR arg) {
    if (!g_traceOn) return;
    LONG i = InterlockedIncrement(&g_traceCount) - 1;
    if (i >= 8192) return;
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);  // our own import: real time
    g_trace[i] = {t.QuadPart, GetCurrentThreadId(), what, arg};
}
const wchar_t* kTraceNames[] = {L"?", L"wait begin", L"wait end", L"SetEvent", L"ReleaseSemaphore", L"PRESENT",
                                L"xinput poll", L"Sleep", L"TICK START"};
void TraceDump() {
    LONGLONG t0 = g_trace[0].t;
    for (LONG i = 0; i < g_traceCount && i < 8192; ++i)
        Log(L"Trace %8.3f ms  thread %5lu  %s %p", (g_trace[i].t - t0) * 1000.0 / g_freq.QuadPart, g_trace[i].thread,
            kTraceNames[g_trace[i].what], (void*)g_trace[i].arg);
}

// ---- fixed clock (main thread) -----------------------------------------------------------------------
const LONGLONG kClockBaseSeconds = 1000;  // same absolute time on every PC
LONGLONG g_callsThisTick = 0;
LONGLONG g_statQpcCalls = 0;  // stats
decltype(&GetTickCount) Real_GetTickCount;
using timeGetTime_t = DWORD(WINAPI*)();
timeGetTime_t Real_timeGetTime;
using srand_t = void(__cdecl*)(unsigned);
srand_t Real_srand;

bool FixedClock() { return g_phase != Phase::Off && GetCurrentThreadId() == g_mainThread; }

BOOL WINAPI Hook_QPC(LARGE_INTEGER* out) {
    if (!FixedClock()) return Real_QPC(out);
    // base + tick x 1/60 s, plus one count per call so "wait until time passes" loops still finish.
    ++g_statQpcCalls;
    out->QuadPart = kClockBaseSeconds * g_freq.QuadPart + (LONGLONG)g_tick * g_freq.QuadPart / 60 +
                    (++g_callsThisTick);
    return TRUE;
}
DWORD FixedMs() { return (DWORD)(kClockBaseSeconds * 1000 + (ULONGLONG)g_tick * 1000 / 60); }
DWORD WINAPI Hook_GetTickCount() { return FixedClock() ? FixedMs() : Real_GetTickCount(); }
DWORD WINAPI Hook_timeGetTime() { return FixedClock() ? FixedMs() : Real_timeGetTime(); }
void __cdecl Hook_srand(unsigned) { Real_srand(0x46475559); }  // "FGUY"

// The menus react to mouse movement; during netplay the local mouse must not change anything.
decltype(&GetCursorPos) Real_GetCursorPos;
BOOL WINAPI Hook_GetCursorPos(POINT* pt) {
    if (!pt) return FALSE;
    pt->x = 0;
    pt->y = 0;
    return TRUE;
}

// ---- quiescence ---------------------------------------------------------------------------------------
// Every game thread that waits/sleeps through the exe's imports is tracked; "idle" while inside the call.
struct ThreadState { volatile DWORD tid; volatile LONG idle; HANDLE handle; };
ThreadState g_threads[64];
volatile LONG g_threadCount = 0;

decltype(&WaitForSingleObject) Real_WaitForSingleObject;

ThreadState* Me() {
    DWORD me = GetCurrentThreadId();
    LONG n = g_threadCount;
    for (LONG i = 0; i < n; ++i)
        if (g_threads[i].tid == me) {
            ThreadState& t = g_threads[i];
            if (t.handle && Real_WaitForSingleObject(t.handle, 0) == WAIT_OBJECT_0) {  // id reused by a new thread
                CloseHandle(t.handle);
                t.handle = OpenThread(THREAD_QUERY_INFORMATION | SYNCHRONIZE, FALSE, me);
            }
            return &t;
        }
    LONG i = InterlockedIncrement(&g_threadCount) - 1;
    if (i >= 64) { InterlockedDecrement(&g_threadCount); return nullptr; }
    g_threads[i].handle = OpenThread(THREAD_QUERY_INFORMATION | SYNCHRONIZE, FALSE, me);
    g_threads[i].tid = me;
    return &g_threads[i];
}
struct IdleScope {  // marks the calling (non-main) thread idle while it blocks
    ThreadState* t;
    IdleScope() : t(g_enabled && GetCurrentThreadId() != g_mainThread ? Me() : nullptr) {
        if (t) InterlockedExchange(&t->idle, 1);
    }
    ~IdleScope() { if (t) InterlockedExchange(&t->idle, 0); }
};

decltype(&Sleep) Real_Sleep;
decltype(&SleepEx) Real_SleepEx;
decltype(&SetEvent) Real_SetEvent;
decltype(&ReleaseSemaphore) Real_ReleaseSemaphore;
decltype(&ReadFile) Real_ReadFile;

// Research: the game thread blocking many times inside one tick (time stands still there).
int g_mainBlocksThisTick = 0;
void NoteMainBlock(const wchar_t* what, DWORD ms, void* caller) {
    if (GetCurrentThreadId() != g_mainThread || g_phase != Phase::Running) return;
    int n = ++g_mainBlocksThisTick;
    if (n == 50 || n == 1000 || n == 20000)
        Log(L"Netplay: tick %u: game thread %s(%lu) #%d this tick, from %p (clock reads %lld)", g_tick, what, ms, n,
            caller, g_callsThisTick);
}

DWORD WINAPI Hook_WaitForSingleObject(HANDLE h, DWORD ms) {
    NoteMainBlock(L"WaitForSingleObject", ms, _ReturnAddress());
    IdleScope idle;
    Trace(1, (ULONG_PTR)h);
    DWORD r = Real_WaitForSingleObject(h, ms);
    Trace(2, (ULONG_PTR)h);
    return r;
}
void WINAPI Hook_Sleep(DWORD ms) {
    NoteMainBlock(L"Sleep", ms, _ReturnAddress());
    Trace(7, ms);
    IdleScope idle;
    Real_Sleep(ms);
}
DWORD WINAPI Hook_SleepEx(DWORD ms, BOOL alertable) {
    NoteMainBlock(L"SleepEx", ms, _ReturnAddress());
    IdleScope idle;
    return Real_SleepEx(ms, alertable);
}
BOOL WINAPI Hook_SetEvent(HANDLE h) { Trace(3, (ULONG_PTR)h); return Real_SetEvent(h); }
BOOL WINAPI Hook_ReleaseSemaphore(HANDLE h, LONG count, LONG* previous) {
    Trace(4, (ULONG_PTR)h);
    return Real_ReleaseSemaphore(h, count, previous);
}

// Blocking socket calls (the game's leftover developer link waits in accept() on TCP 7534 forever).
using accept_t = SOCKET(WSAAPI*)(SOCKET, sockaddr*, int*);
using select_t = int(WSAAPI*)(int, fd_set*, fd_set*, fd_set*, const timeval*);
using recv_t = int(WSAAPI*)(SOCKET, char*, int, int);
using recvfrom_t = int(WSAAPI*)(SOCKET, char*, int, int, sockaddr*, int*);
accept_t Real_accept;
select_t Real_select;
recv_t Real_recv;
recvfrom_t Real_recvfrom;
SOCKET WSAAPI Hook_accept(SOCKET s, sockaddr* a, int* n) { IdleScope idle; return Real_accept(s, a, n); }
int WSAAPI Hook_select(int n, fd_set* r, fd_set* w, fd_set* e, const timeval* t) {
    IdleScope idle;
    return Real_select(n, r, w, e, t);
}
int WSAAPI Hook_recv(SOCKET s, char* b, int n, int f) { IdleScope idle; return Real_recv(s, b, n, f); }
int WSAAPI Hook_recvfrom(SOCKET s, char* b, int n, int f, sockaddr* a, int* l) {
    IdleScope idle;
    return Real_recvfrom(s, b, n, f, a, l);
}

// Background file reads: finish them before ReadFile returns, so disk timing can't shift a tick.
BOOL WINAPI Hook_ReadFile(HANDLE f, LPVOID buf, DWORD n, LPDWORD read, LPOVERLAPPED ov) {
    BOOL r = Real_ReadFile(f, buf, n, read, ov);
    if (r || !ov || GetLastError() != ERROR_IO_PENDING) return r;
    DWORD got = 0;
    if (!GetOverlappedResult(f, ov, &got, TRUE)) return FALSE;
    if (ov->hEvent) SetEvent(ov->hEvent);  // stays signalled for the game's own wait
    if (read) *read = got;
    return TRUE;
}

void* StartAddress(HANDLE h) {
    using NtQIT = LONG(WINAPI*)(HANDLE, int, void*, ULONG, ULONG*);
    static auto q = (NtQIT)GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtQueryInformationThread");
    void* start = nullptr;
    if (q && h) q(h, 9 /* ThreadQuerySetWin32StartAddress */, &start, sizeof(start), nullptr);
    return start;
}

// Research: where is a thread that never parks in a hooked wait? Log its stack's code addresses.
void LogStack(DWORD tid) {
    HANDLE h = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, tid);
    if (!h || SuspendThread(h) == (DWORD)-1) {
        Log(L"Netplay: can't inspect thread %lu (%lu)", tid, GetLastError());
        if (h) CloseHandle(h);
        return;
    }
    CONTEXT ctx{};
    ctx.ContextFlags = CONTEXT_CONTROL;
    if (GetThreadContext(h, &ctx)) {
        auto sp = reinterpret_cast<ULONG_PTR*>(ctx.Esp);
        wchar_t line[1024];
        int len = swprintf_s(line, L"Netplay: busy thread eip %08lx stack:", ctx.Eip);
        for (int i = 0; i < 256 && len < 960; ++i) {
            if (IsBadReadPtr(sp + i, 4)) break;
            ULONG_PTR v = sp[i];
            HMODULE m = nullptr;
            if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                    (LPCWSTR)v, &m) || !m) continue;
            wchar_t name[MAX_PATH];
            GetModuleFileNameW(m, name, MAX_PATH);
            const wchar_t* base = wcsrchr(name, L'\\');
            len += swprintf_s(line + len, 1024 - len, L" %s+%lx", base ? base + 1 : name, (unsigned long)(v - (ULONG_PTR)m));
        }
        Log(L"%s", line);
    } else {
        Log(L"Netplay: can't read thread context (%lu)", GetLastError());
    }
    ResumeThread(h);
    CloseHandle(h);
}

// Stats per 600 ticks (where the time goes).
LONGLONG g_statPeer = 0, g_statQuiet = 0;
int g_statTimeouts = 0;

void WaitForQuiescence() {
    LARGE_INTEGER t0, t;
    Real_QPC(&t0);
    for (;;) {
        bool all = true;
        LONG n = g_threadCount;
        int busy = -1;
        for (LONG i = 0; i < n && all; ++i) {
            ThreadState& th = g_threads[i];
            if (th.idle || th.tid == g_renderThread) continue;
            if (th.handle && Real_WaitForSingleObject(th.handle, 0) == WAIT_OBJECT_0) {  // it has exited
                th.idle = 1;
                continue;
            }
            all = false;
            busy = i;
        }
        if (all) return;
        Real_QPC(&t);
        if (t.QuadPart - t0.QuadPart > g_freq.QuadPart / 20) {  // 50 ms: don't stall forever
            static int warned = 0;
            if (warned++ < 10)
                Log(L"Netplay: thread %lu (start %p) stayed busy at the start of tick %u", g_threads[busy].tid,
                    StartAddress(g_threads[busy].handle), g_tick);
            if (warned <= 3) LogStack(g_threads[busy].tid);
            ++g_statTimeouts;
            return;
        }
        YieldProcessor();
    }
}

// ---- packets ------------------------------------------------------------------------------------------
// All little-endian. 'R' ready: slot. 'I' inputs: slot, first tick, count, Pad[count]. 'H' hash: slot,
// tick, hash. 'S' skip the scene that's holding the game: slot, sequence number.
#pragma pack(push, 1)
struct PktHeader { char type; BYTE slot; };
struct PktInputs { PktHeader h; unsigned first; BYTE count; Pad pads[16]; };
struct PktHash { PktHeader h; unsigned tick; unsigned hash; };
struct PktSkip { PktHeader h; unsigned seq; };
#pragma pack(pop)

void SendAll(const void* data, int len) {
    for (auto& p : g_peers) sendto(g_sock, (const char*)data, len, 0, (const sockaddr*)&p, sizeof(p));
}

unsigned g_peerHash[4][8];           // recent hashes from peers, by (tick / syncEvery) % 8
unsigned g_peerHashTick[4][8];
unsigned g_myHash[8], g_myHashTick[8];
int g_desyncs = 0;

void LogPlayers(const wchar_t* tag);

void CompareHash(int slot, unsigned tick, unsigned hash) {
    int k = (tick / g_syncEvery) % 8;
    if (g_myHashTick[k] != tick) return;
    if (g_myHash[k] == hash) {
        if (tick % (g_syncEvery * 10) == 0) Log(L"Netplay: tick %u in sync with peer %d (%08x)", tick, slot, hash);
    } else {
        ++g_desyncs;
        Log(L"Netplay: DESYNC at tick %u: mine %08x, peer %d has %08x", tick, g_myHash[k], slot, hash);
        if (g_desyncs == 1) LogPlayers(L"first desync, at");
    }
}

CRITICAL_SECTION g_netLock;  // Receive() runs on the game thread, and on the render thread during scenes
unsigned g_skipSeqSeen[4];
void StartSkip(const wchar_t* why);

void Receive() {
    char buf[1500];
    EnterCriticalSection(&g_netLock);
    struct Unlock { ~Unlock() { LeaveCriticalSection(&g_netLock); } } unlock;
    for (;;) {
        int n = recvfrom(g_sock, buf, sizeof(buf), 0, nullptr, nullptr);
        if (n < (int)sizeof(PktHeader)) return;
        auto h = reinterpret_cast<PktHeader*>(buf);
        int slot = h->slot & 3;
        if (h->type == 'R') {
            if (!g_peerReady[slot]) Log(L"Netplay: peer with slot %d is ready", slot);
            g_peerReady[slot] = true;
        } else if (h->type == 'I' && n >= (int)offsetof(PktInputs, pads)) {
            auto p = reinterpret_cast<PktInputs*>(buf);
            for (int i = 0; i < p->count && i < 16; ++i) {
                unsigned f = p->first + i;
                if (f + kRing / 2 < g_tick) continue;  // far too old
                int r = f % kRing;
                g_inputs[r][slot] = p->pads[i];
                g_have[r][slot] = true;
                g_haveTick[r][slot] = f;
            }
        } else if (h->type == 'H' && n >= (int)sizeof(PktHash)) {
            auto p = reinterpret_cast<PktHash*>(buf);
            int k = (p->tick / g_syncEvery) % 8;
            g_peerHash[slot][k] = p->hash;
            g_peerHashTick[slot][k] = p->tick;
            CompareHash(slot, p->tick, p->hash);
        } else if (h->type == 'S' && n >= (int)sizeof(PktSkip)) {
            auto p = reinterpret_cast<PktSkip*>(buf);
            if (p->seq > g_skipSeqSeen[slot]) {
                g_skipSeqSeen[slot] = p->seq;
                StartSkip(L"a peer skipped");
            }
        }
    }
}

bool HaveAll(unsigned tick) {
    int r = tick % kRing;
    for (int s = 0; s < g_players; ++s)
        if (!g_have[r][s] || g_haveTick[r][s] != tick) return false;
    return true;
}

void SendInputs(unsigned upTo) {
    PktInputs p{};
    p.h = {'I', (BYTE)g_slot};
    unsigned first = upTo >= 15 ? upTo - 15 : 0;  // resend the last 16 ticks (packet loss)
    p.first = first;
    p.count = (BYTE)(upTo - first + 1);
    for (unsigned f = first; f <= upTo; ++f) p.pads[f - first] = g_inputs[f % kRing][g_slot];
    SendAll(&p, (int)offsetof(PktInputs, pads) + p.count * (int)sizeof(Pad));
}

// ---- local input ----------------------------------------------------------------------------------------
using XInputGetState_t = DWORD(WINAPI*)(DWORD, XINPUT_STATE*);
XInputGetState_t g_realXInput = nullptr;
unsigned g_playStartTick = 0;        // test script: first tick with players in the level

Pad ScriptPad(unsigned f) {
    // A fixed little routine, timed from when the level's players exist: (slot 0) press A to leave the
    // pause menu the level starts in, (slot 1) press Start to drop in, then walk around and jump/attack.
    Pad p{};
    if (!g_playStartTick || f < g_playStartTick) return p;
    f -= g_playStartTick;
    if (g_slot == 0 && f >= 120 && f < 123) p.buttons |= XINPUT_GAMEPAD_A;
    if (g_idleScript) return p;
    if (g_slot == 1 && f >= 160 && f < 163) p.buttons |= XINPUT_GAMEPAD_START;
    unsigned t = f % 1200;
    if (f > 240) {
        if (t >= 120 && t < 360) p.ly = 30000;
        if (t >= 360 && t < 480) p.lx = 30000;
        if (t >= 480 && t < 720) p.ly = -30000;
        if (t >= 720 && t < 840) p.lx = -30000;
        if (t % 97 < 3) p.buttons |= XINPUT_GAMEPAD_A;
        if (t % 53 < 3) p.buttons |= XINPUT_GAMEPAD_X;
    }
    return p;
}

Pad SampleLocal(unsigned f) {
    if (g_script) return ScriptPad(f);
    Pad p{};
    XINPUT_STATE st{};
    if (g_realXInput && g_realXInput(0, &st) == ERROR_SUCCESS) {  // the player's own controller
        p.buttons = st.Gamepad.wButtons;
        p.lt = st.Gamepad.bLeftTrigger;
        p.rt = st.Gamepad.bRightTrigger;
        p.lx = st.Gamepad.sThumbLX;
        p.ly = st.Gamepad.sThumbLY;
        p.rx = st.Gamepad.sThumbRX;
        p.ry = st.Gamepad.sThumbRY;
    }
    p.buttons |= VPadButtons(0);  // test tools press this player's buttons through the pipe
    return p;
}

// ---- state hash -------------------------------------------------------------------------------------------
// The player spy remembers every player object it ever saw, including deleted ones (e.g. character
// previews in the menus): only live players count (still the player class, index 0..3).
const ULONG_PTR kPlayerVtable = 0xa1af14;
bool LivePlayer(unsigned char* self) {
    if (IsBadReadPtr(self, 0xe00)) return false;
    if (*reinterpret_cast<ULONG_PTR*>(self) != kPlayerVtable) return false;
    unsigned index = *reinterpret_cast<unsigned*>(self + 0xdf8);
    return index < 4 && !IsBadReadPtr(*reinterpret_cast<void**>(self + 0x38), 0x40);
}

unsigned HashPlayers() {
    unsigned h = 2166136261u;
    void* players[8];
    int n = ResearchPlayers(players, 8);
    for (int i = 0; i < n; ++i) {
        auto self = static_cast<unsigned char*>(players[i]);
        if (!LivePlayer(self)) continue;
        auto xform = *reinterpret_cast<unsigned char**>(self + 0x38);
        int index = *reinterpret_cast<int*>(self + 0xdf8);
        h = (h ^ (unsigned)index) * 16777619u;
        for (int k = 0; k < 12; ++k) h = (h ^ xform[0x30 + k]) * 16777619u;
    }
    return h;
}

void LogPlayers(const wchar_t* tag) {
    void* players[8];
    int n = ResearchPlayers(players, 8);
    for (int i = 0; i < n; ++i) {
        auto self = static_cast<unsigned char*>(players[i]);
        if (!LivePlayer(self)) continue;
        auto xform = *reinterpret_cast<float**>(self + 0x38);
        Log(L"Netplay: %s tick %u player %d at (%.4f, %.4f, %.4f)", tag, g_tick,
            *reinterpret_cast<int*>(self + 0xdf8), xform[12], xform[13], xform[14]);
    }
}

volatile ULONGLONG g_tickStartMs = 0;
DWORD WINAPI Watchdog(LPVOID);

// ---- tick boundary ----------------------------------------------------------------------------------------
void Barrier() {
    // Hold the game's first tick until every peer has reached it.
    Log(L"Netplay: first tick - waiting for the other players");
    g_peerReady[g_slot] = true;
    PktHeader r{'R', (BYTE)g_slot};
    ULONGLONG until = GetTickCount64() + 120000;
    for (;;) {
        SendAll(&r, sizeof(r));
        Receive();
        bool all = true;
        for (int s = 0; s < g_players; ++s) all &= g_peerReady[s];
        if (all || GetTickCount64() > until) break;
        Real_Sleep(10);
    }
    for (int i = 0; i < 10; ++i) { SendAll(&r, sizeof(r)); Real_Sleep(5); }  // let late peers see us
    for (unsigned f = 0; f < (unsigned)g_delay; ++f)                       // the first ticks: everyone idle
        for (int s = 0; s < g_players; ++s) {
            g_inputs[f % kRing][s] = Pad{};
            g_have[f % kRing][s] = true;
            g_haveTick[f % kRing][s] = f;
        }
    g_phase = Phase::Running;
    CloseHandle(CreateThread(nullptr, 0, Watchdog, nullptr, 0, nullptr));
    Log(L"Netplay: all %d players ready - lockstep from the first tick (slot %d, input delay %d ticks)",
        g_players, g_slot, g_delay);
}

// Research watchdog: if a tick runs long, show where the game thread is.
DWORD WINAPI Watchdog(LPVOID) {
    for (;;) {
        Real_Sleep(2000);
        ULONGLONG start = g_tickStartMs;
        if (start && GetTickCount64() - start > 15000) {  // longer than any skipped scene
            Log(L"Netplay: tick %u running for %llu ms - game thread:", g_tick, GetTickCount64() - start);
            LogStack(g_mainThread);
            Real_Sleep(30000);
        }
    }
}

// ---- skipping scenes ---------------------------------------------------------------------------------------
// Some scenes (e.g. the loading screen in Stewie's weapon closet, which waits for A once the level has
// loaded) hold the game thread inside one tick while the render thread shows them and polls the controller. Those polls see a skip
// press only when any player skips: the press is sent to everyone, so every copy skips together.
const ULONGLONG kSceneAfterMs = 500;     // a tick this long means a scene is holding the game
volatile ULONGLONG g_skipPressUntil = 0;
ULONGLONG g_skipSendUntil = 0, g_skipNextSend = 0;
unsigned g_skipSeq = 0;
WORD g_localButtonsLast = 0;

void StartSkip(const wchar_t* why) {
    ULONGLONG now = GetTickCount64();
    if (now < g_skipPressUntil + 300) return;  // one press per skip
    g_skipPressUntil = now + 80;
    Log(L"Netplay: skipping the scene at tick %u (%s)", g_tick, why);
}

// Render-thread controller poll while a scene holds the game: the buttons it should see.
WORD ScenePad() {
    ULONGLONG now = GetTickCount64();
    ULONGLONG start = g_tickStartMs;
    bool scene = g_phase == Phase::Running && start && now - start > kSceneAfterMs;
    if (scene && TryEnterCriticalSection(&g_netLock)) {
        Receive();  // the game thread isn't receiving while it's held
        WORD buttons = 0;
        if (g_script) {
            if (g_slot == 0 && (now - start) % 2000 > 1500) buttons = XINPUT_GAMEPAD_A;  // tap every 2 s
        } else if (g_realXInput) {
            XINPUT_STATE st{};
            if (g_realXInput(0, &st) == ERROR_SUCCESS) buttons = st.Gamepad.wButtons;
            buttons |= VPadButtons(0);
        }
        WORD pressed = buttons & ~g_localButtonsLast;
        g_localButtonsLast = buttons;
        if (pressed & (XINPUT_GAMEPAD_A | XINPUT_GAMEPAD_START) && now >= g_skipPressUntil + 300) {
            ++g_skipSeq;
            g_skipSendUntil = now + 300;
            StartSkip(L"skipped here");
        }
        if (now < g_skipSendUntil && now >= g_skipNextSend) {  // a few copies against packet loss
            PktSkip pk{{'S', (BYTE)g_slot}, g_skipSeq};
            SendAll(&pk, sizeof(pk));
            g_skipNextSend = now + 50;
        }
        LeaveCriticalSection(&g_netLock);
    }
    return scene && now < g_skipPressUntil ? XINPUT_GAMEPAD_A : 0;
}

// Called on the main thread at the start of every game tick (its first controller poll).
void Tick() {
    static ULONGLONG lastStart = 0;
    ULONGLONG nowMs = GetTickCount64();
    if (lastStart && nowMs - lastStart > 1000)
        Log(L"Netplay: tick %u took %llu ms (%lld clock reads, %d waits/sleeps)", g_tick, nowMs - lastStart,
            g_callsThisTick, g_mainBlocksThisTick);
    lastStart = nowMs;
    EnterCriticalSection(&g_netLock);
    struct Unlock { ~Unlock() { LeaveCriticalSection(&g_netLock); } } unlock;
    g_tickStartMs = nowMs;
    Receive();
    if (g_phase == Phase::Boot) {
        Barrier();
    } else {
        // The previous tick g_tick is complete: sample state at this exact boundary.
        if (!g_playStartTick) {
            void* players[8];
            if (ResearchPlayers(players, 8) > 0) {
                g_playStartTick = g_tick + 1;
                Log(L"Netplay: players exist from tick %u", g_playStartTick);
            }
        }
        if (g_syncEvery > 0 && g_tick % g_syncEvery == 0) {
            unsigned h = HashPlayers();
            int k = (g_tick / g_syncEvery) % 8;
            g_myHash[k] = h;
            g_myHashTick[k] = g_tick;
            PktHash p{{'H', (BYTE)g_slot}, g_tick, h};
            SendAll(&p, sizeof(p));
            for (int s = 0; s < g_players; ++s)
                if (s != g_slot && g_peerHashTick[s][k] == g_tick) CompareHash(s, g_tick, g_peerHash[s][k]);
            if (g_tick % (g_syncEvery * 10) == 0) LogPlayers(L"check");
        }
        ++g_tick;
    }
    g_mainBlocksThisTick = 0;
    g_callsThisTick = 0;  // the clock reads exactly base + tick x 1/60 s from here

    unsigned future = g_tick + g_delay - 1;
    int r = future % kRing;
    g_inputs[r][g_slot] = SampleLocal(future);
    g_have[r][g_slot] = true;
    g_haveTick[r][g_slot] = future;
    SendInputs(future);

    LARGE_INTEGER s0, s1, s2;
    Real_QPC(&s0);
    ULONGLONG until = GetTickCount64() + 10000;
    ULONGLONG nextResend = GetTickCount64() + 5;
    while (!HaveAll(g_tick)) {
        Receive();
        if (GetTickCount64() >= nextResend) { SendInputs(future); nextResend = GetTickCount64() + 5; }
        if (GetTickCount64() > until) {
            Log(L"Netplay: no input from a peer for 10 s at tick %u - continuing without it", g_tick);
            int q = g_tick % kRing;
            for (int s = 0; s < g_players; ++s)
                if (!g_have[q][s] || g_haveTick[q][s] != g_tick) {
                    g_inputs[q][s] = Pad{}; g_have[q][s] = true; g_haveTick[q][s] = g_tick;
                }
            break;
        }
        Real_Sleep(0);
    }
    for (int s = 0; s < g_players; ++s) g_current[s] = g_inputs[g_tick % kRing][s];
    Real_QPC(&s1);
    WaitForQuiescence();
    Real_QPC(&s2);
    g_statPeer += s1.QuadPart - s0.QuadPart;
    g_statQuiet += s2.QuadPart - s1.QuadPart;

    if (g_traceFrom && g_tick == g_traceFrom) g_traceOn = true;
    if (g_traceFrom && g_tick == g_traceFrom + 4 && g_traceOn) { g_traceOn = false; TraceDump(); }
    Trace(8, g_tick);
    if (g_tick % 600 == 0) {
        static ULONGLONG last = GetTickCount64();
        ULONGLONG now = GetTickCount64();
        Log(L"Netplay: tick %u (%.1f ticks/s over the last 600; per tick: %.2f ms waiting for peers, %.2f ms for "
            L"threads, %d timeouts, %lld clock reads)", g_tick, 600000.0 / (now - last + 1),
            g_statPeer * 1000.0 / g_freq.QuadPart / 600, g_statQuiet * 1000.0 / g_freq.QuadPart / 600, g_statTimeouts,
            g_statQpcCalls / 600);
        last = now;
        g_statPeer = g_statQuiet = g_statQpcCalls = 0;
        g_statTimeouts = 0;
    }
}

}  // namespace

// Called by vpad.cpp's XInputGetState hook. Returns true if netplay supplies this pad's state.
bool NetplayPad(DWORD pad, XINPUT_STATE* state, DWORD(WINAPI* real)(DWORD, XINPUT_STATE*)) {
    if (!g_enabled) return false;
    g_realXInput = real;
    Trace(6, pad);
    if (pad == 0 && GetCurrentThreadId() == g_mainThread) Tick();  // a new game tick starts
    if (pad >= 4 || (int)pad >= g_players) return true;              // unused slots: "not connected"
    static DWORD packet[2][4];
    static Pad last[2][4];
    bool game = GetCurrentThreadId() == g_mainThread;
    Pad p{};
    if (game && g_phase == Phase::Running) p = g_current[pad];
    if (!game && pad == 0) p.buttons = ScenePad();
    if (memcmp(&p, &last[game][pad], sizeof(p)) != 0) { last[game][pad] = p; ++packet[game][pad]; }
    ZeroMemory(state, sizeof(*state));
    state->dwPacketNumber = packet[game][pad];
    state->Gamepad.wButtons = p.buttons;
    state->Gamepad.bLeftTrigger = p.lt;
    state->Gamepad.bRightTrigger = p.rt;
    state->Gamepad.sThumbLX = p.lx;
    state->Gamepad.sThumbLY = p.ly;
    state->Gamepad.sThumbRX = p.rx;
    state->Gamepad.sThumbRY = p.ry;
    return true;
}
bool NetplayPadConnected(DWORD pad) { return g_enabled && pad < 4 && (int)pad < g_players; }

// Called at the end of every Present on the render thread. Netplay itself is driven by the main
// thread's ticks; here we only learn which thread renders (it's excluded from quiescence).
void NetplayOnFrame() {
    if (!g_enabled) return;
    if (!g_renderThread) {
        g_renderThread = GetCurrentThreadId();
        Log(L"Netplay: render thread %lu, game thread %lu", g_renderThread, g_mainThread);
    }
    Trace(5, 0);
}

bool NetplayEnabled() { return g_enabled; }

void InitNetplay(HMODULE exe) {
    g_enabled = SettingInt(L"Netplay", L"Enable", L"FGUYMOD_NET", 0) != 0;
    if (!g_enabled) return;
    g_mainThread = GetCurrentThreadId();  // DllMain runs on the game's main thread
    g_slot = SettingInt(L"Netplay", L"Slot", L"FGUYMOD_NET_SLOT", 0) & 3;
    g_players = SettingInt(L"Netplay", L"Players", L"FGUYMOD_NET_PLAYERS", 2);
    g_delay = SettingInt(L"Netplay", L"InputDelay", L"FGUYMOD_NET_DELAY", 3);
    g_script = SettingInt(L"Netplay", L"Script", L"FGUYMOD_NET_SCRIPT", 0) != 0;
    g_idleScript = SettingInt(L"Netplay", L"_", L"FGUYMOD_NET_IDLE", 0) != 0;
    g_traceFrom = (unsigned)SettingInt(L"Netplay", L"_", L"FGUYMOD_NET_TRACE", 0);
    g_syncEvery = SettingInt(L"Netplay", L"SyncCheckFrames", nullptr, 60);
    for (int s = 0; s < 4; ++s)
        for (int k = 0; k < 8; ++k) g_peerHashTick[s][k] = ~0u;
    for (int k = 0; k < 8; ++k) g_myHashTick[k] = ~0u;
    int port = SettingInt(L"Netplay", L"Port", L"FGUYMOD_NET_PORT", 7600);
    wchar_t peers[512];
    SettingString(L"Netplay", L"Peers", L"FGUYMOD_NET_PEERS", L"", peers, 512);

    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
    g_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    sockaddr_in me{};
    me.sin_family = AF_INET;
    me.sin_port = htons((u_short)port);
    bind(g_sock, (sockaddr*)&me, sizeof(me));
    u_long nonblocking = 1;
    ioctlsocket(g_sock, FIONBIO, &nonblocking);
    wchar_t* ctx = nullptr;
    for (wchar_t* tok = wcstok_s(peers, L",", &ctx); tok; tok = wcstok_s(nullptr, L",", &ctx)) {
        wchar_t host[128];
        int peerPort = 0;
        if (swscanf_s(tok, L" %127[^:]:%d", host, (unsigned)_countof(host), &peerPort) != 2) continue;
        sockaddr_in a{};
        a.sin_family = AF_INET;
        a.sin_port = htons((u_short)peerPort);
        InetPtonW(AF_INET, host, &a.sin_addr);
        g_peers.push_back(a);
    }
    QueryPerformanceFrequency(&g_freq);
    InitializeCriticalSection(&g_netLock);
    g_phase = Phase::Boot;
    Log(L"Netplay: on - slot %d of %d players, UDP port %d, %zu peers, input delay %d, %s input",
        g_slot, g_players, port, g_peers.size(), g_delay, g_script ? L"scripted" : L"controller");

    HookSpec hooks[] = {
        {"kernel32.dll", "QueryPerformanceCounter", (void*)Hook_QPC, (void**)&Real_QPC},
        {"kernel32.dll", "GetTickCount", (void*)Hook_GetTickCount, (void**)&Real_GetTickCount},
        {"winmm.dll", "timeGetTime", (void*)Hook_timeGetTime, (void**)&Real_timeGetTime},
        {"msvcr100.dll", "srand", (void*)Hook_srand, (void**)&Real_srand},
        {"user32.dll", "GetCursorPos", (void*)Hook_GetCursorPos, (void**)&Real_GetCursorPos},
        {"kernel32.dll", "WaitForSingleObject", (void*)Hook_WaitForSingleObject, (void**)&Real_WaitForSingleObject},
        {"kernel32.dll", "SetEvent", (void*)Hook_SetEvent, (void**)&Real_SetEvent},
        {"kernel32.dll", "ReleaseSemaphore", (void*)Hook_ReleaseSemaphore, (void**)&Real_ReleaseSemaphore},
        {"kernel32.dll", "Sleep", (void*)Hook_Sleep, (void**)&Real_Sleep},
        {"kernel32.dll", "SleepEx", (void*)Hook_SleepEx, (void**)&Real_SleepEx},
        {"kernel32.dll", "ReadFile", (void*)Hook_ReadFile, (void**)&Real_ReadFile},
        // ws2_32 ordinals: 1 accept, 18 select, 16 recv, 17 recvfrom
        {"ws2_32.dll", MAKEINTRESOURCEA(1), (void*)Hook_accept, (void**)&Real_accept},
        {"ws2_32.dll", MAKEINTRESOURCEA(18), (void*)Hook_select, (void**)&Real_select},
        {"ws2_32.dll", MAKEINTRESOURCEA(16), (void*)Hook_recv, (void**)&Real_recv},
        {"ws2_32.dll", MAKEINTRESOURCEA(17), (void*)Hook_recvfrom, (void**)&Real_recvfrom},
    };
    InstallHooks(exe, hooks);
}
