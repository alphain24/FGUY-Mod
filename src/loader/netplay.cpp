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
//   * Local keyboard / mouse / cursor don't reach the game directly (dinput.cpp, Hook_GetCursorPos); they
//     play as this player's controller instead (kbm.cpp), so they go through the lockstep like a pad.
// Every SyncCheckFrames ticks peers compare a hash of the players' positions and aim, taken at the tick boundary.
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
#include <string>
#include <vector>

#pragma comment(lib, "ws2_32.lib")

namespace {

// ---- settings ---------------------------------------------------------------------------------------
bool g_enabled = false;
int g_slot = 0, g_players = 2, g_delay = 3, g_syncEvery = 60;
// Ticks (= frames) per second, the same for everyone. A session boots, walks the menus and loads the level at
// 60 (the game's join screens don't take a press at higher rates), then switches to the lobby's frame rate
// (g_rateTarget) once everyone is in the level - the same tick on every PC (g_rateFrom).
int g_rate = 60;
int g_rateTarget = 60;
unsigned g_rateFrom = 0;             // the tick the match's rate took over (0: still 60)
bool g_script = false, g_idleScript = false;
unsigned g_traceFrom = 0;
std::vector<sockaddr_in> g_peers;

// ---- state ------------------------------------------------------------------------------------------
struct Pad {
    WORD buttons;
    BYTE lt, rt;
    SHORT lx, ly, rx, ry;
    KbmRaw kbm;          // keyboard / mouse players: their raw keyboard and mouse (kbm.cpp)
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

bool g_allClock = false;   // research: FGUYMOD_TEST_ALLCLOCK - every thread but the render thread on the fixed clock
bool FixedClock() {
    if (g_phase == Phase::Off) return false;
    DWORD me = GetCurrentThreadId();
    return me == g_mainThread || (g_allClock && me != g_renderThread);
}
// The fixed clock's time at a tick, in 1 / `unit` s: 60 per second up to g_rateFrom, g_rate per second after.
LONGLONG TickTime(unsigned tick, LONGLONG unit) {
    if (!g_rateFrom || tick < g_rateFrom) return (LONGLONG)tick * unit / 60;
    return (LONGLONG)g_rateFrom * unit / 60 + (LONGLONG)(tick - g_rateFrom) * unit / g_rate;
}
// After leaving a session (back to the lobby): real time again, shifted so the game thread's clock carries on
// from the fixed one instead of jumping.
bool g_left = false;
LONGLONG g_qpcOffset = 0;
LONGLONG g_tickCountOffset = 0, g_timeGetTimeOffset = 0;
bool Shifted() { return g_left && GetCurrentThreadId() == g_mainThread; }

BOOL WINAPI Hook_QPC(LARGE_INTEGER* out) {
    if (!FixedClock()) {
        BOOL r = Real_QPC(out);
        if (r && out && Shifted()) out->QuadPart += g_qpcOffset;
        return r;
    }
    // base + the tick's time, plus one count per call so "wait until time passes" loops still finish (the game
    // thread's own count: other threads' reads mustn't change what it sees)
    LONGLONG at = kClockBaseSeconds * g_freq.QuadPart + TickTime(g_tick, g_freq.QuadPart);
    if (GetCurrentThreadId() != g_mainThread) {
        static thread_local LONGLONG mine = 0;   // still moving forward for this thread
        mine = mine < at ? at : mine + 1;
        out->QuadPart = mine;
        return TRUE;
    }
    ++g_statQpcCalls;
    out->QuadPart = at + (++g_callsThisTick);
    return TRUE;
}
DWORD FixedMs() { return (DWORD)(kClockBaseSeconds * 1000 + TickTime(g_tick, 1000)); }
DWORD WINAPI Hook_GetTickCount() {
    return FixedClock() ? FixedMs() : (DWORD)(Real_GetTickCount() + (Shifted() ? g_tickCountOffset : 0));
}
DWORD WINAPI Hook_timeGetTime() {
    return FixedClock() ? FixedMs() : (DWORD)(Real_timeGetTime() + (Shifted() ? g_timeGetTimeOffset : 0));
}
void __cdecl Hook_srand(unsigned) { Real_srand(0x46475559); }  // "FGUY"

// The menus react to mouse movement; during netplay the local mouse must not change anything.
decltype(&GetCursorPos) Real_GetCursorPos;
BOOL WINAPI Hook_GetCursorPos(POINT* pt) {
    if (!pt) return FALSE;
    if (!g_enabled) return Real_GetCursorPos(pt);   // after leaving a session
    if (KbmCursorScreen(pt)) return TRUE;  // keyboard players' cursor, through the lockstep (kbm.cpp)
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
// tick, hash. 'S' skip the scene that's holding the game: slot, sequence number. 'L' left the match (back to
// the lobby): slot, the last tick with their input - from the next one on they're idle on every PC.
#pragma pack(push, 1)
struct PktHeader { char type; BYTE slot; };
struct PktInputs { PktHeader h; unsigned first; BYTE count; Pad pads[16]; };
struct PktHash { PktHeader h; unsigned tick; unsigned hash; };
struct PktSkip { PktHeader h; unsigned seq; };
struct PktLeave { PktHeader h; unsigned last; };
#pragma pack(pop)

// Relay mode (online sessions): every packet goes to the lobby server, wrapped as
// 'F' 'G' <8-byte session token> <our slot> <packet>; the server forwards it to the other players.
bool g_relay = false;
unsigned char g_token[8];

volatile ULONGLONG g_testStallUntil = 0;   // test pipe "netstall <ms>": this PC sends nothing until then

void SendAll(const void* data, int len) {
    if (g_testStallUntil && GetTickCount64() < g_testStallUntil) return;
    if (g_relay) {
        char buf[1500];
        if (len > (int)sizeof(buf) - 11) return;
        buf[0] = 'F';
        buf[1] = 'G';
        memcpy(buf + 2, g_token, 8);
        buf[10] = (char)g_slot;
        memcpy(buf + 11, data, len);
        sendto(g_sock, buf, len + 11, 0, (const sockaddr*)&g_peers[0], sizeof(g_peers[0]));
        return;
    }
    for (auto& p : g_peers) sendto(g_sock, (const char*)data, len, 0, (const sockaddr*)&p, sizeof(p));
}

unsigned g_peerHash[4][8];           // recent hashes from peers, by (tick / syncEvery) % 8
unsigned g_peerHashTick[4][8];
unsigned g_myHash[8], g_myHashTick[8];
int g_desyncs = 0;
// What each check hashed, kept so a mismatch can show the exact values of that tick (both PCs log the same tick).
struct Hashed { int count; int index[8]; float pos[8][3], aim[8][3]; };
Hashed g_hashed[8];


void LogPlayers(const wchar_t* tag);

void NoteMismatch(unsigned tick);

// The players' names, by slot (FGUYMOD_NET_NAMES from the lobby: "Host|Guest|..."), for the notices.
std::wstring PlayerName(int slot) {
    static wchar_t names[4][40];
    static bool read = false;
    if (!read) {
        read = true;
        char env[200];
        if (GetEnvironmentVariableA("FGUYMOD_NET_NAMES", env, sizeof(env))) {
            int s = 0;
            for (char* p = env; s < 4;) {
                char* bar = strchr(p, '|');
                if (bar) *bar = 0;
                MultiByteToWideChar(CP_UTF8, 0, p, -1, names[s], 40);
                ++s;
                if (!bar) break;
                p = bar + 1;
            }
        }
    }
    if (slot < 0 || slot >= 4) return L"";
    if (names[slot][0]) return names[slot];
    wchar_t n[16];
    swprintf_s(n, L"Player %d", slot + 1);
    return n;
}

void CompareHash(int slot, unsigned tick, unsigned hash) {
    int k = (tick / g_syncEvery) % 8;
    if (g_myHashTick[k] != tick) return;
    static bool wasInSync = true;
    if (g_myHash[k] != hash) NoteMismatch(tick);
    if (g_myHash[k] == hash) {
        if (tick % (g_syncEvery * 10) == 0 || !wasInSync) Log(L"Netplay: tick %u in sync with peer %d (%08x)", tick, slot, hash);
        wasInSync = true;
    } else {
        ++g_desyncs;
        Log(L"Netplay: DESYNC at tick %u: mine %08x, peer %d has %08x", tick, g_myHash[k], slot, hash);
        bool details = g_desyncs <= 3 || wasInSync;   // the first ones, and each time it goes out of sync
        wasInSync = false;
        if (details) {   // what this PC hashed at that tick (compare with the peer's log)
            const Hashed& v = g_hashed[k];
            for (int i = 0; i < v.count; ++i)
                Log(L"Netplay:   tick %u player %d pos (%.5f, %.5f, %.5f) aim (%.5f, %.5f, %.5f)", tick, v.index[i],
                    v.pos[i][0], v.pos[i][1], v.pos[i][2], v.aim[i][0], v.aim[i][1], v.aim[i][2]);
        }
    }
}

CRITICAL_SECTION g_netLock;  // Receive() runs on the game thread, and on the render thread during scenes
unsigned g_goneFrom[4];      // a player who left the match: idle from this tick on (0: still playing)
unsigned g_skipSeqSeen[4];
void StartSkip(const wchar_t* why);

void Receive() {
    char buf[1500];
    EnterCriticalSection(&g_netLock);
    struct Unlock { ~Unlock() { LeaveCriticalSection(&g_netLock); } } unlock;
    for (;;) {
        int n = recvfrom(g_sock, buf, sizeof(buf), 0, nullptr, nullptr);
        if (n == SOCKET_ERROR && WSAGetLastError() == WSAECONNRESET) continue;  // ICMP noise, not an error
        if (n == SOCKET_ERROR) return;  // nothing more to read
        if (g_relay) {  // only our session's packets, through the server
            if (n < 11 + (int)sizeof(PktHeader) || buf[0] != 'F' || buf[1] != 'G' || memcmp(buf + 2, g_token, 8) != 0)
                continue;
            memmove(buf, buf + 11, n - 11);
            n -= 11;
        }
        if (n < (int)sizeof(PktHeader)) continue;
        auto h = reinterpret_cast<PktHeader*>(buf);
        int slot = h->slot;
        if (slot >= g_players || slot == g_slot) continue;  // not a peer's (our own inputs are never replaced)
        if (h->type == 'R') {
            if (!g_peerReady[slot]) Log(L"Netplay: peer with slot %d is ready", slot);
            g_peerReady[slot] = true;
        } else if (h->type == 'I' && n >= (int)offsetof(PktInputs, pads)) {
            auto p = reinterpret_cast<PktInputs*>(buf);
            int count = (n - (int)offsetof(PktInputs, pads)) / (int)sizeof(Pad);  // what actually arrived
            if (count > p->count) count = p->count;
            for (int i = 0; i < count && i < 16; ++i) {
                unsigned f = p->first + i;
                if (f + kRing / 2 < g_tick || f > g_tick + kRing / 2) continue;  // far too old / too far ahead
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
        } else if (h->type == 'L' && n >= (int)sizeof(PktLeave)) {
            auto p = reinterpret_cast<PktLeave*>(buf);
            if (!g_goneFrom[slot] || p->last + 1 < g_goneFrom[slot]) {
                g_goneFrom[slot] = p->last + 1;
                Log(L"Netplay: player %d left the match (back to the lobby) - idle from tick %u", slot, p->last + 1);
            }
        } else if (h->type == 'S' && n >= (int)sizeof(PktSkip)) {
            auto p = reinterpret_cast<PktSkip*>(buf);
            if (p->seq > g_skipSeqSeen[slot]) {
                g_skipSeqSeen[slot] = p->seq;
                StartSkip(L"a peer skipped");
            }
        }
    }
}

bool g_gone[4];                      // a player who stopped sending (left): their pad stays idle

bool HaveAll(unsigned tick) {
    int r = tick % kRing;
    for (int s = 0; s < g_players; ++s) {
        if (g_goneFrom[s] && tick >= g_goneFrom[s]) {   // left the match: idle (whatever arrived)
            g_inputs[r][s] = Pad{};
            g_have[r][s] = true;
            g_haveTick[r][s] = tick;
            continue;
        }
        if (g_have[r][s] && g_haveTick[r][s] == tick) continue;
        if (!g_gone[s]) return false;
        g_inputs[r][s] = Pad{};
        g_have[r][s] = true;
        g_haveTick[r][s] = tick;
    }
    return true;
}

unsigned g_lastSent = 0;             // the last tick our inputs went out for

void SendInputs(unsigned upTo) {
    g_lastSent = upTo;
    PktInputs p{};
    p.h = {'I', (BYTE)g_slot};
    unsigned first = upTo >= 7 ? upTo - 7 : 0;  // resend the last 8 ticks (packet loss; ~27 KB/s per player)
    p.first = first;
    p.count = (BYTE)(upTo - first + 1);
    for (unsigned f = first; f <= upTo; ++f) p.pads[f - first] = g_inputs[f % kRing][g_slot];
    SendAll(&p, (int)offsetof(PktInputs, pads) + p.count * (int)sizeof(Pad));
}

// ---- local input ----------------------------------------------------------------------------------------
using XInputGetState_t = DWORD(WINAPI*)(DWORD, XINPUT_STATE*);
XInputGetState_t g_realXInput = nullptr;
unsigned g_playStartTick = 0;        // test script: first tick with players in the level
bool g_inSession = false;            // an online session (the lobby started it)
unsigned g_liveTick = 0;             // session: first tick every player's level HUD was up (in their inputs)

// The test bot in an online session: from a second after everyone's level is up, walk around, look about,
// jump, shoot in bursts and reload now and then (a different rhythm per slot).
Pad BotPad(unsigned f) {
    Pad p{};
    if (!g_liveTick || f < g_liveTick + 60) return p;
    f = f - g_liveTick + (unsigned)g_slot * 377;
    unsigned t = f % 960;
    if (t < 240) p.ly = 32000;
    else if (t < 360) { p.ly = 20000; p.lx = 26000; }
    else if (t < 600) p.ly = -28000;
    else if (t < 720) p.lx = -30000;
    else p.ly = 24000;
    if ((f / 90) % 3 == 1) p.rx = (f / 270) % 2 ? 14000 : -14000;   // look around
    if (f % 150 < 4) p.buttons |= XINPUT_GAMEPAD_A;                 // jump
    if (f % 200 < 40) p.rt = 255;                                    // shoot in bursts
    if (f % 1500 < 4) p.buttons |= XINPUT_GAMEPAD_X;                // reload
    return p;
}

// Out of sync for good (3 checks running, once the level has settled - a mismatch while everyone drops in fixes
// itself): each PC sees it at the same checks, says so and takes everyone back to the lobby.
void NoteMismatch(unsigned tick) {
    static unsigned lastBadTick = 0;
    static int badRun = 0;
    if (!g_inSession || !g_liveTick || tick <= g_liveTick + 600 || tick == lastBadTick) return;
    badRun = lastBadTick && tick - lastBadTick <= (unsigned)g_syncEvery ? badRun + 1 : 1;
    lastBadTick = tick;
    if (badRun == 3) {
        Log(L"Netplay: out of sync since tick %u - back to the lobby", tick - 2 * g_syncEvery);
        OverlayShow(L"The match went out of sync - back to the lobby", 6000);
        OnlinePauseForceQuit();
    }
}

Pad ScriptPad(unsigned f) {
    if (g_inSession) return BotPad(f);
    // Direct netplay tests (no lobby): a fixed little routine, timed from when the level's players exist:
    // (slot 0) press A to leave the pause menu the level starts in, (slot 1) press Start to drop in, then
    // walk around and jump/attack.
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
    if (g_script) {
        Pad sp = ScriptPad(f);
        if (HandoffHudUp()) sp.kbm.flags |= 1;   // my level's HUD is up (as below)
        return sp;
    }
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
    if (KbmIsKeyboardSlot(g_slot)) KbmCapture(&p.kbm);  // keyboard / mouse players: the raw state (kbm.cpp)
    if (HandoffHudUp()) p.kbm.flags |= 1;              // my level's HUD is up (kbm.cpp waits for everyone's)
    if (g_inSession && OnlinePauseSample(&p.buttons, &p.lx, &p.ly, &p.kbm, KbmIsKeyboardSlot(g_slot))) {
        p.lt = p.rt = 0;   // my pause menu is open: my character stands still (onlinepause.cpp)
        p.lx = p.rx = p.ry = 0;
    }
    if (g_inSession) OnlineSettingsStamp(&p.kbm);   // my aim settings go with my input (onlinesettings.cpp)
    static Pad last{};
    static int logged = 0;
    if (memcmp(&p, &last, sizeof(p)) != 0 && logged < 60) {  // the first few changes (input problems)
        ++logged;
        last = p;
        int keys = 0;
        for (BYTE b : p.kbm.keys) for (; b; b &= b - 1) ++keys;
        Log(L"Netplay: my input for tick %u: buttons %04x sticks L(%d, %d) R(%d, %d) triggers %d %d; keys %d, mouse "
            L"(%d, %d, %d) buttons %x", f, p.buttons, p.lx, p.ly, p.rx, p.ry, p.lt, p.rt, keys, p.kbm.dx, p.kbm.dy,
            p.kbm.dz, p.kbm.buttons);
    }
    return p;
}

// ---- state hash -------------------------------------------------------------------------------------------
// The player spy remembers every player object it ever saw, including deleted ones (e.g. character
// previews in the menus): only live players count (still the player class, index 0..3).
const ULONG_PTR kPlayerVtable = 0xa1af14;
// What the sync check compares per player (found by watching memory while players run and look around,
// tools/re/memwatch.py): the position (+0x38 points to the player's transform, position at +0x30) and the
// camera / aim point (+0xf88 points to an object with it at +0xcc), which follows the player's camera.
const int kXform = 0x38, kXformPos = 0x30, kAim = 0xf88, kAimPos = 0xcc;
const float* PlayerPos(unsigned char* self) {
    auto xform = *reinterpret_cast<unsigned char**>(self + kXform);
    return IsBadReadPtr(xform, kXformPos + 12) ? nullptr : reinterpret_cast<const float*>(xform + kXformPos);
}
const float* PlayerAim(unsigned char* self) {
    int viewport = *reinterpret_cast<int*>(self + 0xdfc);  // only players with a view of their own have one
    if (viewport < 0 || viewport > 3) return nullptr;       // (others can point at leftover memory)
    auto aim = *reinterpret_cast<unsigned char**>(self + kAim);
    return IsBadReadPtr(aim, kAimPos + 12) ? nullptr : reinterpret_cast<const float*>(aim + kAimPos);
}

bool LivePlayer(unsigned char* self) {
    if (IsBadReadPtr(self, kAim + 4)) return false;
    if (*reinterpret_cast<ULONG_PTR*>(self) != kPlayerVtable) return false;
    unsigned index = *reinterpret_cast<unsigned*>(self + 0xdf8);
    return index < 4 && PlayerPos(self);
}

}  // namespace

// The game's own list of players in the level (0xbad988: up to 8 pointers, count at +0x20), live ones only,
// in player-index order - the same set and order on every PC. (The research player spy's list is in the
// order this PC happened to notice each object, holds leftovers from the menus, and fills up.)
int GamePlayers(unsigned char** out, int max) {
    auto list = *reinterpret_cast<unsigned char* volatile*>(0xbad988);
    if (!list || IsBadReadPtr(list, 0x24)) return 0;
    int count = *reinterpret_cast<int*>(list + 0x20), n = 0;
    for (int i = 0; i < count && i < 8 && n < max; ++i) {
        auto p = reinterpret_cast<unsigned char**>(list)[i];
        if (!p || !LivePlayer(p)) continue;
        int k = n++;
        while (k > 0 && *reinterpret_cast<int*>(out[k - 1] + 0xdf8) > *reinterpret_cast<int*>(p + 0xdf8)) {
            out[k] = out[k - 1];
            --k;
        }
        out[k] = p;
    }
    return n;
}

namespace {

unsigned HashPlayers(Hashed* keep) {
    unsigned h = 2166136261u;
    unsigned char* players[8];
    int n = GamePlayers(players, 8);
    keep->count = n;
    for (int i = 0; i < n; ++i) {
        auto self = players[i];
        auto pos = reinterpret_cast<const unsigned char*>(PlayerPos(self));
        auto aim = reinterpret_cast<const unsigned char*>(PlayerAim(self));
        keep->index[i] = *reinterpret_cast<int*>(self + 0xdf8);
        memcpy(keep->pos[i], pos, 12);
        if (aim) memcpy(keep->aim[i], aim, 12); else memset(keep->aim[i], 0, 12);
        int index = *reinterpret_cast<int*>(self + 0xdf8);
        h = (h ^ (unsigned)index) * 16777619u;
        for (int k = 0; k < 12; ++k) h = (h ^ pos[k]) * 16777619u;
        for (int k = 0; aim && k < 12; ++k) h = (h ^ aim[k]) * 16777619u;
    }
    return h;
}

void LogPlayers(const wchar_t* tag) {
    unsigned char* players[8];
    int n = GamePlayers(players, 8);
    for (int i = 0; i < n; ++i) {
        const float* pos = PlayerPos(players[i]);
        const float* aim = PlayerAim(players[i]);
        static const float none[3] = {0, 0, 0};
        if (!aim) aim = none;
        Log(L"Netplay: %s tick %u player %d at (%.4f, %.4f, %.4f) aim (%.3f, %.3f, %.3f)", tag, g_tick,
            *reinterpret_cast<int*>(players[i] + 0xdf8), pos[0], pos[1], pos[2], aim[0], aim[1], aim[2]);
    }
}

volatile ULONGLONG g_tickStartMs = 0;
DWORD WINAPI Watchdog(LPVOID);

// ---- session autopilot presses ----------------------------------------------------------------------------
// In an online session the menu scripts get everyone's game from boot to the match by themselves
// (src/ui/session.as2js): they ask for button presses with FGUYMOD_SessionPress. A request made while a
// frame's menus run is taken at the start of the next tick - the same tick on every PC, because the next
// tick can't start before that frame is done - and pressed for a few ticks on every copy alike.
struct SessionPress { int pad; WORD buttons; int delay; unsigned from; };
CRITICAL_SECTION g_pressLock;
std::vector<SessionPress> g_pressQueue, g_pressActive;
bool g_sessionStory = false;     // story co-op session: guests drop in once the level runs
volatile LONG g_sessionLevelAsked = 0;  // the menu launched the level (taken at the next tick)
unsigned g_levelTick = 0;

void TakeSessionPresses() {      // main thread, start of a tick
    EnterCriticalSection(&g_pressLock);
    for (auto& p : g_pressQueue) {
        p.from = g_tick + p.delay;
        g_pressActive.push_back(p);
    }
    g_pressQueue.clear();
    LeaveCriticalSection(&g_pressLock);
    for (size_t i = 0; i < g_pressActive.size();) {
        const SessionPress& p = g_pressActive[i];
        if (g_tick >= p.from + (unsigned)(4 * g_rate / 60)) {   // held 1/15 s, whatever the frame rate
            g_pressActive.erase(g_pressActive.begin() + i);
            continue;
        }
        if (g_tick >= p.from && p.pad >= 0 && p.pad < 4) g_current[p.pad].buttons |= p.buttons;
        ++i;
    }
    if (InterlockedExchange(&g_sessionLevelAsked, 0)) g_levelTick = g_tick;
    // story co-op: once the level runs, every other player presses Start to drop in, again every 3 s until
    // the game has given them their viewport (the viewport count is game state, the same on every PC)
    const int kViewportCount = 0xb53e60;
    const unsigned wait = 10 * g_rate, every = 3 * g_rate;   // 10 s in, then every 3 s
    if (g_sessionStory && g_levelTick && g_tick >= g_levelTick + wait && (g_tick - g_levelTick - wait) % every == 0) {
        int views = *reinterpret_cast<volatile int*>(kViewportCount);
        for (int s = 1; s < g_players; ++s)
            if (views < s + 1) {
                g_pressActive.push_back({s, XINPUT_GAMEPAD_START, 0, g_tick});
                g_current[s].buttons |= XINPUT_GAMEPAD_START;
                Log(L"Netplay: session - player %d drops in (tick %u)", s, g_tick);
                break;  // one at a time, in slot order
            }
    }
}

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
        if (KbmSkipPressed()) buttons |= XINPUT_GAMEPAD_A;  // Space / Enter / Esc skip too
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

// Real time keeps step with the fixed 1/60 s clock: each tick starts no sooner than 1/60 s after the last.
// (Here rather than at Present: a paused game, or one whose window can't be seen, stops presenting, and
// then ran the match several times too fast.) Not while an online session boots hidden: that runs flat out.
void PaceTick() {
    static LONGLONG next = 0;
    if (g_phase != Phase::Running || HandoffBooting()) {
        next = 0;
        return;
    }
    LARGE_INTEGER now;
    Real_QPC(&now);
    LONGLONG step = g_freq.QuadPart / g_rate;
    if (!next) next = now.QuadPart;
    if (now.QuadPart < next) {
        LONGLONG ms = (next - now.QuadPart) * 1000 / g_freq.QuadPart;
        if (ms > 1) Real_Sleep((DWORD)(ms - 1));
        do Real_QPC(&now); while (now.QuadPart < next);
    }
    next += step;
    if (now.QuadPart - next > g_freq.QuadPart / 4) next = now.QuadPart;  // far behind: don't race to catch up
}

// Called on the main thread at the start of every game tick (its first controller poll).
void Tick() {
    PaceTick();
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
            unsigned char* players[8];
            if (GamePlayers(players, 8) > 0) {
                g_playStartTick = g_tick + 1;
                Log(L"Netplay: players exist from tick %u", g_playStartTick);
            }
        }
        if (g_syncEvery > 0 && g_tick % g_syncEvery == 0) {
            int k = (g_tick / g_syncEvery) % 8;
            unsigned h = HashPlayers(&g_hashed[k]);
            g_myHash[k] = h;
            g_myHashTick[k] = g_tick;
            PktHash p{{'H', (BYTE)g_slot}, g_tick, h};
            SendAll(&p, sizeof(p));
            for (int s = 0; s < g_players; ++s)   // (not with a player who has left: they're idle here from then)
                if (s != g_slot && !g_gone[s] && !g_goneFrom[s] && g_peerHashTick[s][k] == g_tick)
                    CompareHash(s, g_tick, g_peerHash[s][k]);
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
    ULONGLONG waitFrom = GetTickCount64(), until = waitFrom + 10000, nextNotice = waitFrom + 400;
    ULONGLONG nextResend = GetTickCount64() + 5;
    bool noticed = false;
    while (!HaveAll(g_tick)) {
        Receive();
        ULONGLONG now = GetTickCount64();
        if (now >= nextResend) { SendInputs(future); nextResend = now + 5; }
        if (g_inSession && now >= nextNotice) {   // held up: say who we're waiting for (the game draws nothing now)
            nextNotice = now + 250;
            noticed = true;
            std::wstring who;
            int q = g_tick % kRing;
            for (int s = 0; s < g_players; ++s)
                if (s != g_slot && !g_gone[s] && (!g_have[q][s] || g_haveTick[q][s] != g_tick))
                    who += (who.empty() ? L"" : L", ") + PlayerName(s);
            wchar_t text[160];
            swprintf_s(text, L"Waiting for %s... (%d s)", who.empty() ? L"the others" : who.c_str(),
                       (int)((until - now + 999) / 1000));
            OverlayShow(text, 0);
        }
        if (now > until) {  // they've left (or their connection is gone): play on without them
            int q = g_tick % kRing;
            for (int s = 0; s < g_players; ++s)
                if (!g_have[q][s] || g_haveTick[q][s] != g_tick) {
                    g_gone[s] = true;
                    Log(L"Netplay: no input from player %d for 10 s at tick %u - they left; their character stays idle", s, g_tick);
                    if (g_inSession) OverlayShow((PlayerName(s) + L" left the match").c_str(), 5000);
                }
            HaveAll(g_tick);
            noticed = false;
            break;
        }
        Real_Sleep(0);
    }
    if (noticed) OverlayHide();
    for (int s = 0; s < g_players; ++s) g_current[s] = g_inputs[g_tick % kRing][s];
    TakeSessionPresses();
    KbmRaw kbm[4] = {};
    for (int s = 0; s < g_players && s < 4; ++s) kbm[s] = g_current[s].kbm;
    if (g_inSession && !g_liveTick) {  // every player says their level's HUD is up: the level is live
        bool all = g_players > 0;
        for (int s = 0; s < g_players && s < 4; ++s) all &= (kbm[s].flags & 1) != 0;
        if (all) {
            g_liveTick = g_tick;
            Log(L"Netplay: everyone's level is up (tick %u)", g_tick);
        }
    }
    if (g_inSession && NetplayLevelLive()) {  // everyone's pause menu open: pause the game (onlinepause.cpp)
        BYTE flags[4] = {};
        bool gone[4] = {};
        for (int s = 0; s < g_players && s < 4; ++s) {
            flags[s] = g_current[s].kbm.flags;
            gone[s] = g_gone[s] || (g_goneFrom[s] && g_tick >= g_goneFrom[s]);
        }
        bool keyboard = false;
        int at = OnlinePauseTick(flags, gone, g_players, &keyboard);
        if (at >= 0 && at < 4) {
            if (keyboard) g_current[at].kbm.keys[VK_ESCAPE >> 3] |= (BYTE)(1 << (VK_ESCAPE & 7));
            else g_current[at].buttons |= XINPUT_GAMEPAD_START;
            kbm[at] = g_current[at].kbm;
        }
    }
    // everyone's in, and the spawn has settled (players fall into the level a tick apart at first and only come
    // together again once they've landed - at 60): the match's frame rate
    if (g_rate != g_rateTarget && g_liveTick && g_tick >= g_liveTick + 600) {
        const int kViewportCount = 0xb53e60;   // (story co-op: once everyone has dropped in)
        if (!g_sessionStory || *reinterpret_cast<volatile int*>(kViewportCount) >= g_players) {
            g_rateFrom = g_tick;   // this tick's time is the same either way: no jump
            g_rate = g_rateTarget;
            Log(L"Netplay: %d frames per second from tick %u", g_rate, g_tick);
        }
    }
    if (g_inSession) OnlineSettingsApply(kbm, g_players);   // everyone's own aim settings (onlinesettings.cpp)
    KbmApply(kbm, g_players);  // keyboard / mouse players' devices, from their raw input (kbm.cpp)
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

bool g_levelLaunched = false;   // this session's level was launched (from then on, the front end = the match is over)

void NetplaySessionLevel() {
    if (g_enabled) InterlockedExchange(&g_sessionLevelAsked, 1);
    g_levelLaunched = true;
}

// Leave the session (back to the lobby): tell the others the last tick with our input (they play on with
// us idle), then run on our own - real clock (carrying on from the fixed one), real controllers, keyboard and
// mouse straight to the game again.
void NetplayLeave(const wchar_t* why) {
    if (!g_enabled) return;
    {
        EnterCriticalSection(&g_netLock);
        PktLeave p{{'L', (BYTE)g_slot}, g_lastSent};
        for (int i = 0; i < 6; ++i) {   // our last inputs and the notice, a few times (packets get lost)
            SendInputs(g_lastSent);
            SendAll(&p, sizeof(p));
            Real_Sleep(4);
        }
        LARGE_INTEGER real;
        Real_QPC(&real);
        g_qpcOffset = kClockBaseSeconds * g_freq.QuadPart + TickTime(g_tick, g_freq.QuadPart) +
                      (++g_callsThisTick) - real.QuadPart;
        g_tickCountOffset = (LONGLONG)FixedMs() - (LONGLONG)Real_GetTickCount();
        g_timeGetTimeOffset = (LONGLONG)FixedMs() - (LONGLONG)Real_timeGetTime();
        g_left = true;
        g_tickStartMs = 0;   // (the watchdog: no more ticks)
        g_phase = Phase::Off;
        g_enabled = false;
        LeaveCriticalSection(&g_netLock);
    }
    KbmLeaveSession();
    Log(L"Netplay: left the session at tick %u (%s) - this game runs on its own now", g_tick, why);
}

// The menus, when a page of the front end opens: is the match over (back to the lobby)? Yes once this
// session's level has been launched - the front end only comes back after the match (its results) or a quit.
bool NetplaySessionReturn() {
    static bool returned = false;
    if (returned) return true;
    if (!g_inSession || !g_levelLaunched) return false;
    returned = true;
    NetplayLeave(L"the match is over");
    char code[16] = "", token[40] = "";
    GetEnvironmentVariableA("FGUYMOD_NET_LOBBY", code, sizeof(code));
    GetEnvironmentVariableA("FGUYMOD_NET_TOKEN", token, sizeof(token));
    if (code[0] && token[0]) OnlineRejoin(code, token, g_slot);   // (if the lobby has to open here after all)
    HandoffGiveBack();   // the player's own game takes over again, back in the lobby
    return true;
}

void NetplaySessionPress(int pad, WORD buttons, int delayTicks) {
    if (!g_enabled) return;
    if (delayTicks < 0) delayTicks = 0;
    delayTicks = delayTicks * g_rate / 60;   // the menus' delays are in 1/60 s
    EnterCriticalSection(&g_pressLock);
    g_pressQueue.push_back({pad, buttons, delayTicks, 0});
    LeaveCriticalSection(&g_pressLock);
    Log(L"Netplay: session press pad %d buttons %04x in %d ticks", pad, buttons, delayTicks);
}
int NetplayLocalSlot() { return g_enabled ? g_slot : -1; }
void NetplayTestStall(DWORD ms) {
    g_testStallUntil = GetTickCount64() + ms;
    Log(L"Netplay: test - sending nothing for %lu ms", ms);
}
int NetplaySessionSlot() { return g_slot; }
unsigned NetplayTick() { return g_tick; }
int NetplayRate() { return g_rate; }
void NetplayLogStacks() {
    Log(L"Netplay: game thread %lu:", g_mainThread);
    LogStack(g_mainThread);
    if (g_renderThread) { Log(L"Netplay: render thread %lu:", g_renderThread); LogStack(g_renderThread); }
}
bool NetplayLevelLive() { return g_enabled && g_liveTick && g_tick >= g_liveTick; }

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
    g_rateTarget = SettingInt(L"Netplay", L"TickRate", L"FGUYMOD_NET_RATE", 60);
    if (g_rateTarget < 30 || g_rateTarget > 240) g_rateTarget = 60;
    g_syncEvery = SettingInt(L"Netplay", L"SyncCheckFrames", L"FGUYMOD_TEST_SYNCEVERY", 60);
    g_allClock = SettingInt(L"Netplay", L"_", L"FGUYMOD_TEST_ALLCLOCK", 0) != 0;
    for (int s = 0; s < 4; ++s)
        for (int k = 0; k < 8; ++k) g_peerHashTick[s][k] = ~0u;
    for (int k = 0; k < 8; ++k) g_myHashTick[k] = ~0u;
    int port = SettingInt(L"Netplay", L"Port", L"FGUYMOD_NET_PORT", 7600);
    wchar_t peers[512];
    SettingString(L"Netplay", L"Peers", L"FGUYMOD_NET_PEERS", L"", peers, 512);

    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
    wchar_t relay[256], token[32];
    SettingString(L"Netplay", L"_", L"FGUYMOD_NET_RELAY", L"", relay, 256);
    SettingString(L"Netplay", L"_", L"FGUYMOD_NET_TOKEN", L"", token, 32);
    if (relay[0] && wcslen(token) == 16) {
        g_relay = true;
        for (int i = 0; i < 8; ++i) g_token[i] = (unsigned char)wcstoul(std::wstring(token + 2 * i, 2).c_str(), nullptr, 16);
        port = 0;  // any free local port: the server learns it from our packets
        wcscpy_s(peers, relay);
    }
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
        if (InetPtonW(AF_INET, host, &a.sin_addr) != 1) {  // a name (the lobby server): look it up
            ADDRINFOW hints{}, *res = nullptr;
            hints.ai_family = AF_INET;
            if (GetAddrInfoW(host, nullptr, &hints, &res) != 0 || !res) {
                Log(L"Netplay: can't find %s", host);
                continue;
            }
            a.sin_addr = reinterpret_cast<sockaddr_in*>(res->ai_addr)->sin_addr;
            FreeAddrInfoW(res);
        }
        g_peers.push_back(a);
    }
    QueryPerformanceFrequency(&g_freq);
    InitializeCriticalSection(&g_netLock);
    InitializeCriticalSection(&g_pressLock);
    char session[512];  // (too small a buffer gets nothing copied)
    DWORD sessionLen = GetEnvironmentVariableA("FGUYMOD_SESSION", session, sizeof(session));
    g_sessionStory = sessionLen > 0 && sessionLen < sizeof(session) && strncmp(session, "ST,", 3) == 0;
    g_inSession = sessionLen > 0 && sessionLen < sizeof(session);
    KbmSessionStart(g_inSession ? session : nullptr);
    if (g_inSession) OnlineSettingsInit();   // my own settings (onlinesettings.cpp)
    g_phase = Phase::Boot;
    Log(L"Netplay: %d frames per second in the match (60 until everyone is in the level)", g_rateTarget);
    Log(L"Netplay: on - slot %d of %d players, %s, input delay %d, %s input", g_slot, g_players,
        g_relay ? L"through the lobby server's relay" : L"direct UDP", g_delay, g_script ? L"scripted" : L"controller");

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
