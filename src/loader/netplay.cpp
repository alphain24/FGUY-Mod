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
// Rejoining a match (catch-up): every PC records every tick as it played it - everyone's input, and who was idle - and
// its sync checks. A player who left (or whose game closed) can come back: their new game (FGUYMOD_NET_CATCHUP) asks the
// host for that record ('J' / 'K') and plays the whole match from its first tick on it, hidden and as fast as it can
// (handoff.cpp), checking it comes out as the host's did. Meanwhile their character stays idle for everyone. Once it's
// close, the host's input says "player N is back from tick T" (everyone reads it at the same tick); from T on everyone
// waits for their input again, and their game shows.
//
// Settings ([Netplay] in FGUYMod.ini, env overrides for tests):
//   Enable=1  Slot=0..3  Players=2  Port=7600  Peers=127.0.0.1:7601,...  InputDelay=3
//   Script=1 (scripted test inputs instead of the real pad)  SyncCheckFrames=30
// Research knobs (env only): FGUYMOD_NET_IDLE=1 (script only leaves the start pause),
//   FGUYMOD_NET_TRACE=<tick> (timeline of thread handoffs for 4 ticks).

#include "common.h"

#include <winsock2.h>
#include <ws2tcpip.h>
#include <xinput.h>

#include <intrin.h>

#include <cmath>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <algorithm>
#include <map>
#include <string>
#include <vector>

#pragma comment(lib, "ws2_32.lib")

namespace {

// ---- settings ---------------------------------------------------------------------------------------
bool g_enabled = false;
int g_slot = 0, g_players = 2, g_delay = 3, g_syncEvery = 60;
// Ticks (= frames) per second, the same for everyone at every tick. A session boots, walks the menus and loads
// the level at 60 (the game's join screens don't take a press at higher rates). Once everyone is in the level the
// match follows the PCs: each PC says in its inputs how many frames a second it manages right now (g_paceMine,
// from its own work per tick - not the waiting for the others), and every tick runs at the slowest of those, up
// to the lobby's frame rate (g_rateTarget). Every PC has the same inputs for a tick, so every PC picks the same
// rate for it - and each tick's time step is 1 / that rate, so the game always runs at real speed: a slow PC means
// fewer frames for everyone, never slow motion. g_clockNs: the fixed clock at the start of g_tick.
int g_rate = 60;
int g_rateTarget = 60;
bool g_rateFollows = false;          // the match's rate follows the PCs (from 10 s after everyone's level is up)
int g_paceMine = 0;                  // frames per second this PC manages (0: not measured yet)
LONGLONG g_clockNs = 0;
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
const int kRing = 2048;              // ticks of input kept around the one being played (live inputs a little ahead
                                     // too: catching up, the others are up to a few seconds ahead)
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
extern bool g_free;        // the match runs free (no lockstep): real time, shifted (below)
extern bool g_freeRejoin;  // (rejoining a free-running match: boots on its own)
bool FixedClock() {
    if (g_phase == Phase::Off || g_free) return false;
    DWORD me = GetCurrentThreadId();
    return me == g_mainThread || (g_allClock && me != g_renderThread);
}
// The fixed clock's time at the start of the current tick, in 1 / `unit` s (exact integers: the same on every PC).
LONGLONG TickTime(LONGLONG unit) {
    const LONGLONG kNs = 1000000000;
    return g_clockNs / kNs * unit + g_clockNs % kNs * unit / kNs;
}
// After leaving a session (back to the lobby): real time again, shifted so the game thread's clock carries on
// from the fixed one instead of jumping.
bool g_left = false;
LONGLONG g_qpcOffset = 0;
LONGLONG g_tickCountOffset = 0, g_timeGetTimeOffset = 0;
bool Shifted() { return (g_left || g_free) && GetCurrentThreadId() == g_mainThread; }

BOOL WINAPI Hook_QPC(LARGE_INTEGER* out) {
    if (!FixedClock()) {
        BOOL r = Real_QPC(out);
        if (r && out && Shifted()) out->QuadPart += g_qpcOffset;
        return r;
    }
    // base + the tick's time, plus one count per call so "wait until time passes" loops still finish (the game
    // thread's own count: other threads' reads mustn't change what it sees)
    LONGLONG at = kClockBaseSeconds * g_freq.QuadPart + TickTime(g_freq.QuadPart);
    if (GetCurrentThreadId() != g_mainThread) {
        static thread_local LONGLONG mine = 0;   // still moving forward for this thread
        mine = mine < at ? at : mine + 1;
        out->QuadPart = mine;
        return TRUE;
    }
    ++g_statQpcCalls;
    // The tick's exact time - the same on every PC however often this PC's game reads it (that differs: ~80 a
    // tick). Only a loop waiting for time to pass reads it this often: from then on it moves, so the loop ends.
    const LONGLONG kExactReads = 1000;
    LONGLONG n = ++g_callsThisTick;
    out->QuadPart = at + (n > kExactReads ? n - kExactReads : 0);
    return TRUE;
}
DWORD FixedMs() { return (DWORD)(kClockBaseSeconds * 1000 + TickTime(1000)); }
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
// tick, hash (and its two parts: the players, the random numbers). 'M' the host's random numbers for a tick, in
// three parts; 'Q' asks the host for them again. 'P' ping: slot, the sender's clock (ms); 'O' its echo: the echoing
// slot, to whom, that clock - the round trip, shown in the match (FGUYMOD_NetStats). 'S' skip the scene that's holding the game: slot, sequence number. 'L' left the match (back to
// the lobby): slot, the last tick with their input - from the next one on they're idle on every PC.
// Rejoining: 'J' a rejoining player asks the host for the match's record: from that tick on, how many packets (0: none -
// just where I am), and the tick they're at. 'K' the host's answer, to that player: a stretch of ticks (each as its
// difference from the one before), the host's sync checks in it, and where the host is.
// 'N' a player waiting for my inputs from a tick on (a packet with them got lost): sent again. 'D' a player stopped
// waiting for someone (10 s without their input): everyone plays on without them from that tick, and they go back to the
// lobby (they can rejoin from there).
// Direct connections ([Online] DirectConnection=1, both players): 'a' to the server - I'd like to; the server answers
// 'A' (its slot 0xFE): where a player who'd like it too is, as it sees them; 'Y' straight to them, ack 1 when theirs
// came - from then on everything goes to them straight as well as through the server (whichever comes first counts).
#pragma pack(push, 1)
struct PktHeader { char type; BYTE slot; };
struct PktInputs { PktHeader h; unsigned first; BYTE count; Pad pads[16]; };
struct PktHash { PktHeader h; unsigned tick; unsigned hash; unsigned players, rng; };
const int kRngWords = 624, kRngPart = 208;
struct PktRng { PktHeader h; unsigned tick; BYTE part; int next, left; unsigned words[kRngPart]; };
struct PktRngAsk { PktHeader h; unsigned tick; };
struct PktSkip { PktHeader h; unsigned seq; };
struct PktLeave { PktHeader h; unsigned last; };
struct PktResend { PktHeader h; BYTE to; unsigned from; };      // 'N': player `to`, send me your inputs again from `from`
struct PktDropped { PktHeader h; BYTE who; unsigned from; };    // 'D': I play on without player `who` from tick `from`
struct PktDirect { PktHeader h; BYTE ack; };                     // 'Y': straight to a player (no server) - 'a': to the
struct PktPeerAt { PktHeader h; BYTE who; BYTE ip[4]; BYTE port[2]; };   //  server, I'd connect directly; 'A': from it
struct PktPing { PktHeader h; BYTE to; unsigned long long sent; };
struct PktHistAsk { PktHeader h; unsigned from, at; BYTE want; };
struct PktHist { PktHeader h; BYTE to, hashes, last; unsigned first; WORD count; unsigned hostTick, word; unsigned goneFrom[4]; };
struct HashRec { unsigned tick, hash, players, rng; };   // (in a 'K' after the header: `hashes` of them, then the ticks)
#pragma pack(pop)

// Relay mode (online sessions): every packet goes to the lobby server, wrapped as
// 'F' 'G' <8-byte session token> <our slot> <packet>; the server forwards it to the other players.
bool g_relay = false;
unsigned char g_token[8];

volatile LONG g_testNudge = 0;
volatile LONG g_testResW = 0, g_testResH = 0;   // test pipe "resolution <w> <h>": the game changes resolution   // test pipe "nudge": move my player a little on this PC only (out of sync)
volatile ULONGLONG g_testStallUntil = 0;   // test pipe "netstall <ms>": this PC sends nothing until then

bool g_catchUpFailed = false;        // catching up: given up (this game quits; it says nothing to anyone any more)

// Direct connections to the other players (opt-in: [Online] DirectConnection - and theirs too, the server checks).
struct Direct {
    sockaddr_in addr;                // where they are, as the server sees them
    bool known, ok;                  // ok: their 'Y' came straight to me, and they've had mine
    ULONGLONG punchUntil, nextPunch;
};
Direct g_direct[4];

void SendAll(const void* data, int len) {
    if (g_testStallUntil && GetTickCount64() < g_testStallUntil) return;
    if (g_catchUpFailed) return;
    if (g_relay) {
        char buf[1500];
        if (len > (int)sizeof(buf) - 11) return;
        buf[0] = 'F';
        buf[1] = 'G';
        memcpy(buf + 2, g_token, 8);
        buf[10] = (char)g_slot;
        memcpy(buf + 11, data, len);
        sendto(g_sock, buf, len + 11, 0, (const sockaddr*)&g_peers[0], sizeof(g_peers[0]));
        for (int s = 0; s < 4; ++s)   // (and straight to the players I have a direct connection with)
            if (g_direct[s].ok) sendto(g_sock, buf, len + 11, 0, (const sockaddr*)&g_direct[s].addr, sizeof(sockaddr_in));
        return;
    }
    for (auto& p : g_peers) sendto(g_sock, (const char*)data, len, 0, (const sockaddr*)&p, sizeof(p));
}

unsigned g_peerHash[4][8];           // recent hashes from peers, by (tick / syncEvery) % 8
unsigned g_peerPlayers[4][8], g_peerRng[4][8];
unsigned g_peerHashTick[4][8];
unsigned g_myHash[8], g_myHashTick[8];
int g_desyncs = 0;
// What each check hashed, kept so a mismatch can show the exact values of that tick (both PCs log the same tick).
struct Hashed { int count; int index[8]; float pos[8][3], aim[8][3]; int money[8]; float health[8]; unsigned players, rng; int rngLeft; };
Hashed g_hashed[8];
// Every tick's player state, the last 128 ticks: the first time the match goes out of sync both PCs write the ticks
// since the last check that matched - side by side the logs show the very tick the players parted.
const int kHistory = 128;
Hashed g_history[kHistory];
unsigned g_historyTick[kHistory];


void LogPlayers(const wchar_t* tag);
int PlayerMoney(const unsigned char* player);
float PlayerHealth(const unsigned char* player);

void NoteMismatch(unsigned tick);
void RngDrift(unsigned tick);
struct PktRng;
void RngReceive(const PktRng* p);
void RngAsked(unsigned tick);

// The players' names, by slot (FGUYMOD_NET_NAMES from the lobby: "Host|Guest|..."), for the notices.
wchar_t g_names[4][40];
void ReadNames() {
    static bool read = false;
    wchar_t (&names)[4][40] = g_names;
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
}

std::wstring PlayerName(int slot) {
    ReadNames();
    if (slot < 0 || slot >= 4) return L"";
    if (g_names[slot][0]) return g_names[slot];
    wchar_t n[16];
    swprintf_s(n, L"Player %d", slot + 1);
    return n;
}

void CompareHash(int slot, unsigned tick, unsigned hash, unsigned players, unsigned rng) {
    int k = (tick / g_syncEvery) % 8;
    if (g_myHashTick[k] != tick) return;
    static bool wasInSync = true;
    // Only the random numbers went apart (something on one PC only took one - the players are still the same):
    // everyone takes the host's (RngDrift) and the match goes on. Anything else: out of sync.
    bool onlyRng = g_myHash[k] != hash && players == g_hashed[k].players && rng != g_hashed[k].rng;
    if (g_myHash[k] != hash) {
        if (onlyRng) RngDrift(tick);
        else NoteMismatch(tick);
    }
    if (g_myHash[k] == hash) {
        if (tick % (g_syncEvery * 10) == 0 || !wasInSync) Log(L"Netplay: tick %u in sync with peer %d (%08x)", tick, slot, hash);
        wasInSync = true;
    } else {
        ++g_desyncs;
        Log(L"Netplay: DESYNC at tick %u: mine %08x, peer %d has %08x%s", tick, g_myHash[k], slot, hash,
            onlyRng ? L" - only the random numbers (the players match)" : L"");
        bool details = g_desyncs <= 3 || wasInSync;   // the first ones, and each time it goes out of sync
        wasInSync = false;
        if (details) {   // what this PC hashed at that tick (compare with the peer's log)
            const Hashed& v = g_hashed[k];
            for (int i = 0; i < v.count; ++i)
                Log(L"Netplay:   tick %u player %d pos (%.5f, %.5f, %.5f) aim (%.5f, %.5f, %.5f) money %d health %.5f", tick,
                    v.index[i], v.pos[i][0], v.pos[i][1], v.pos[i][2], v.aim[i][0], v.aim[i][1], v.aim[i][2], v.money[i],
                    v.health[i]);
            Log(L"Netplay:   tick %u random numbers %08x (%d left before the next refill)", tick, v.rng, v.rngLeft);
        }
        static bool historyDone = false;
        if (!historyDone && !onlyRng) {
            historyDone = true;
            for (unsigned t = tick > (unsigned)g_syncEvery + 2 ? tick - g_syncEvery - 2 : 0; t <= tick; ++t) {
                const Hashed& h = g_history[t % kHistory];
                if (g_historyTick[t % kHistory] != t) continue;
                for (int i = 0; i < h.count; ++i)
                    Log(L"Netplay:   history tick %u player %d pos (%.5f, %.5f, %.5f) aim (%.5f, %.5f, %.5f) money %d health %.5f "
                        L"rng %08x", t, h.index[i], h.pos[i][0], h.pos[i][1], h.pos[i][2], h.aim[i][0], h.aim[i][1], h.aim[i][2],
                        h.money[i], h.health[i], h.rng);
            }
        }
    }
}

CRITICAL_SECTION g_netLock;  // Receive() runs on the game thread, and on the render thread during scenes
unsigned g_goneFrom[4];      // a player who left the match: idle from this tick on (0: still playing)
volatile LONG g_pingMs[4] = {-1, -1, -1, -1};   // the round trip to each player (ms; -1: not known yet)
const int kPingRecent = 8;
int g_pingRecent[4][kPingRecent];    // the last few round trips to each player (2 s of them)
unsigned g_pingAt[4];
unsigned g_skipSeqSeen[4];
void StartSkip(const wchar_t* why);

// ---- who plays: players who left, and players coming back --------------------------------------------------------
bool g_gone[4];                      // a player who stopped sending (left): their pad stays idle (until they rejoin)
unsigned g_backAt[4];                // a player rejoining: everyone waits for their input again from this tick on
unsigned g_goneAgain[4];             //   and they left again from this tick on (noticed before they were back)
unsigned g_lastSent = 0;             // the last tick our inputs went out for
bool g_inSession = false;            // an online session (the lobby started it)
void SendInputs(unsigned upTo);
unsigned g_sentUpTo = 0;             // the furthest tick my inputs have gone out for

// A player whose pad is idle at this tick on every PC (left the match, or not back yet).
bool Idle(int s, unsigned tick) {
    if (g_backAt[s]) return tick < g_backAt[s];
    return g_gone[s] || (g_goneFrom[s] && tick >= g_goneFrom[s]);
}

// The start of every tick: players rejoining from this tick on are in the match again.
void BackIn(unsigned tick);

// ---- the match's record (every PC), for a player who rejoins ----------------------------------------------------
// One tick as it was played: who was idle (bit per player), everyone's input.
struct Row { BYTE idle; Pad pads[4]; };

// A stretch of rows, each as its difference from the row before (bytes XOR - nearly all zero), as pairs of
// <zero bytes> <other bytes> <those bytes> (counts as 7-bit varints). Each stretch starts from all-zero rows, so it
// decodes on its own.
struct RowCoder {
    std::vector<unsigned char> out, lit;
    unsigned zeros = 0;
    Row prev{};
    static void Varint(std::vector<unsigned char>& v, unsigned x) {
        for (; x >= 0x80; x >>= 7) v.push_back((unsigned char)(x | 0x80));
        v.push_back((unsigned char)x);
    }
    void Pair() {
        Varint(out, zeros);
        Varint(out, (unsigned)lit.size());
        out.insert(out.end(), lit.begin(), lit.end());
        zeros = 0;
        lit.clear();
    }
    void Byte(unsigned char b) {
        if (b) { lit.push_back(b); return; }
        if (!lit.empty()) Pair();
        ++zeros;
    }
    void Add(const Row& r, int players) {
        Byte(r.idle ^ prev.idle);
        for (int s = 0; s < players; ++s) {
            auto a = reinterpret_cast<const unsigned char*>(&r.pads[s]), b = reinterpret_cast<const unsigned char*>(&prev.pads[s]);
            for (size_t i = 0; i < sizeof(Pad); ++i) Byte(a[i] ^ b[i]);
        }
        prev = r;
    }
    size_t Size() const { return out.size() + lit.size() + 10; }
    std::vector<unsigned char> Finish() const {
        RowCoder c = *this;
        if (c.zeros || !c.lit.empty()) c.Pair();
        return c.out;
    }
};

bool DecodeRows(const unsigned char* d, size_t n, unsigned count, int players, Row* rows) {
    size_t at = 0;
    unsigned zeros = 0, lit = 0;
    auto varint = [&](unsigned& x) {
        x = 0;
        for (int shift = 0; shift < 35; shift += 7) {
            if (at >= n) return false;
            unsigned char c = d[at++];
            x |= (unsigned)(c & 0x7f) << shift;
            if (!(c & 0x80)) return true;
        }
        return false;
    };
    auto next = [&](unsigned char& b) {   // the next difference byte
        while (!zeros && !lit)
            if (!varint(zeros) || !varint(lit) || at + lit > n) return false;
        if (zeros) { --zeros; b = 0; }
        else { --lit; b = d[at++]; }
        return true;
    };
    Row prev{};
    for (unsigned t = 0; t < count; ++t) {
        Row r = prev;
        unsigned char b;
        if (!next(b)) return false;
        r.idle ^= b;
        for (int s = 0; s < players; ++s) {
            auto p = reinterpret_cast<unsigned char*>(&r.pads[s]);
            for (size_t i = 0; i < sizeof(Pad); ++i) {
                if (!next(b)) return false;
                p[i] ^= b;
            }
        }
        rows[t] = prev = r;
    }
    return true;
}

const size_t kHistData = 1000;       // a stretch's bytes at most (it goes in one packet)
const int kHistHashes = 8;           // sync checks in one packet at most
const int kServeMax = 24;            // packets per answer
struct Stretch { unsigned first, count; std::vector<unsigned char> data; };
std::vector<Stretch> g_rec;          // the match so far, in stretches (and the open one below)
RowCoder g_recOpen;
unsigned g_recOpenFirst = 0, g_recOpenCount = 0, g_recNext = 0;
std::vector<HashRec> g_recHashes;    // my sync checks, in tick order
std::vector<PktRng> g_rngAll;        // host: every time everyone took my random numbers, what they were

void Record(const Row& r) {
    unsigned most = 8u * (unsigned)(g_syncEvery > 0 ? g_syncEvery : 30);   // (at most kHistHashes checks in a stretch)
    if (most > 240) most = 240;
    RowCoder more = g_recOpen;
    more.Add(r, g_players);
    if (g_recOpenCount && (g_recOpenCount >= most || more.Size() > kHistData)) {   // full: close it, start the next
        g_rec.push_back({g_recOpenFirst, g_recOpenCount, g_recOpen.Finish()});
        g_recOpen = RowCoder{};
        g_recOpen.Add(r, g_players);
        g_recOpenFirst = g_recNext;
        g_recOpenCount = 1;
    } else {
        if (!g_recOpenCount) g_recOpenFirst = g_recNext;
        g_recOpen = std::move(more);
        ++g_recOpenCount;
    }
    ++g_recNext;
}

// Host: a player rejoining - where they've got to (for when they're back), and the record they asked for.
struct Rejoiner { ULONGLONG heardAt; unsigned at, issued; bool told; };
Rejoiner g_rejoiners[4];

void SendStretch(int to, unsigned first, unsigned count, const std::vector<unsigned char>* data, bool last) {
    char buf[1400];
    auto p = reinterpret_cast<PktHist*>(buf);
    memset(p, 0, sizeof(*p));
    p->h = {'K', (BYTE)g_slot};
    p->to = (BYTE)to;
    p->last = last ? 1 : 0;
    p->first = first;
    p->count = (WORD)count;
    p->hostTick = g_tick;
    p->word = to > 0 && to < 4 ? g_rejoiners[to].issued : 0;   // (the tick of my word that they're back, once given)
    for (int s = 0; s < 4; ++s) p->goneFrom[s] = g_goneFrom[s];
    size_t at = sizeof(PktHist);
    if (count && data) {
        auto it = std::lower_bound(g_recHashes.begin(), g_recHashes.end(), first,
                                   [](const HashRec& h, unsigned t) { return h.tick < t; });
        for (; it != g_recHashes.end() && it->tick < first + count && p->hashes < kHistHashes; ++it) {
            memcpy(buf + at, &*it, sizeof(HashRec));
            at += sizeof(HashRec);
            ++p->hashes;
        }
        if (at + data->size() > sizeof(buf)) {
            static int logged = 0;
            if (logged++ < 3) Log(L"Netplay: a stretch of the match too big to send (%u bytes)", (unsigned)data->size());
            return;
        }
        memcpy(buf + at, data->data(), data->size());
        at += data->size();
    }
    SendAll(buf, (int)at);
}

void ServeHistory(int to, const PktHistAsk* q) {
    if (to <= 0 || to >= g_players || to >= 4) return;
    Rejoiner& r = g_rejoiners[to];
    if (!r.told && (Idle(to, g_tick) || g_backAt[to])) {   // (not for a late ask from someone already back)
        r.told = true;
        Log(L"Netplay: player %d is rejoining the match (at tick %u, the match at %u)", to, q->at, g_tick);
        OverlayShow((PlayerName(to) + L" is rejoining the match...").c_str(), 5000);
    }
    r.heardAt = GetTickCount64();
    r.at = q->at;
    int want = q->want > kServeMax ? kServeMax : q->want, sent = 0;
    bool openNeeded = g_recOpenCount && g_recOpenFirst + g_recOpenCount > q->from;
    // the stretch with `from` in it, and on (the open one last: all of it so far)
    auto it = std::upper_bound(g_rec.begin(), g_rec.end(), q->from, [](unsigned t, const Stretch& x) { return t < x.first; });
    if (it != g_rec.begin() && q->from < (it - 1)->first + (it - 1)->count) --it;
    for (; it != g_rec.end() && sent < want; ++it, ++sent) {
        bool last = sent + 1 == want || (it + 1 == g_rec.end() && !openNeeded);
        SendStretch(to, it->first, it->count, &it->data, last);
    }
    if (sent < want && openNeeded) {
        std::vector<unsigned char> open = g_recOpen.Finish();
        SendStretch(to, g_recOpenFirst, g_recOpenCount, &open, true);
        ++sent;
    }
    if (!sent) SendStretch(to, q->from, 0, nullptr, true);   // (nothing more yet, or not asked for: where I am)
}

// ---- catching up (the rejoining player's game) --------------------------------------------------------------------
bool g_catchUp = false;              // this game rejoins a running match: it plays it from its start on the host's
                                     // record, as fast as it can, until it's caught up (FGUYMOD_NET_CATCHUP)
unsigned g_myBackAt = 0;             //   the tick I'm back in the match from (0: not known yet)
CatchUpShared* g_cuShared = nullptr; //   how far it is, for my lobby game's loading screen
ULONGLONG g_catchUpStartMs = 0, g_histHeardAt = 0;
unsigned g_hostTick = 0;             //   where the match is (the host's last answer)
unsigned g_myWordTick = 0;           //   the tick of the host's word that I'm back (an earlier word for my slot in the
                                     //   record was an earlier rejoin - a game of mine before this one)
struct HistIn { unsigned count; std::vector<unsigned char> data; };
std::map<unsigned, HistIn> g_histIn; //   the record as it came, by first tick
unsigned g_histEdge = 0;             //   every tick before this is in it
bool g_histAsked = false;            //   an answer is on its way
std::map<unsigned, HashRec> g_histHashes;   // the host's sync checks
std::vector<Row> g_histRows;         //   the stretch being played, decoded
unsigned g_histRowsFirst = 0;

void CatchUpFail(const wchar_t* why) {
    if (g_catchUpFailed) return;
    Log(L"Netplay: couldn't rejoin the match - %s (tick %u, the match at %u)", why, g_tick, g_hostTick);
    if (g_myBackAt) {   // the others wait for me from that tick: I've gone again
        PktLeave p{{'L', (BYTE)g_slot}, g_lastSent >= g_myBackAt ? g_lastSent : g_myBackAt - 1};
        for (int i = 0; i < 6; ++i) { SendAll(&p, sizeof(p)); Real_Sleep(4); }
    }
    g_catchUpFailed = true;
    if (g_cuShared) {
        wcsncpy_s(g_cuShared->reason, why, _TRUNCATE);
        InterlockedExchange(&g_cuShared->state, 2);
    }
    VideoRequestQuit();   // (the game runs on, idle and unseen, to its next frame - then quits)
    CloseHandle(CreateThread(nullptr, 0, [](LPVOID) -> DWORD {
        Real_Sleep(10000);
        Log(L"Netplay: no frame since giving up - exiting");
        QuitGame();
        return 0;
    }, nullptr, 0, nullptr));
}

void StoreHistory(const PktHist* p, int n) {
    if (p->to != g_slot) return;
    g_histHeardAt = GetTickCount64();
    if (p->hostTick > g_hostTick) g_hostTick = p->hostTick;
    if (p->last) g_histAsked = false;
    if (p->word) g_myWordTick = p->word;
    for (int s = 0; s < 4 && s < g_players; ++s)   // (players who left: from when - for once I'm back in the match)
        if (s != g_slot && p->goneFrom[s] && !g_goneFrom[s] && !g_backAt[s]) g_goneFrom[s] = p->goneFrom[s];
    size_t at = sizeof(PktHist) + p->hashes * sizeof(HashRec);
    if (at > (size_t)n) return;
    for (int i = 0; i < p->hashes; ++i) {
        HashRec h;
        memcpy(&h, reinterpret_cast<const char*>(p) + sizeof(PktHist) + i * sizeof(HashRec), sizeof(h));
        if (h.tick >= g_tick) g_histHashes[h.tick] = h;
    }
    if (!p->count || p->first + p->count <= g_tick) return;   // (nothing in it, or played already)
    HistIn& e = g_histIn[p->first];
    if (p->count > e.count) {
        e.count = p->count;
        e.data.assign(reinterpret_cast<const unsigned char*>(p) + at, reinterpret_cast<const unsigned char*>(p) + n);
    }
    for (;;) {   // how far it goes without a gap
        auto it = g_histIn.upper_bound(g_histEdge);
        if (it == g_histIn.begin()) break;
        --it;
        unsigned end = it->first + it->second.count;
        if (end <= g_histEdge) break;
        g_histEdge = end;
    }
}

// The host's record of a tick (false: not here yet).
bool HistRow(unsigned t, Row* out) {
    if (t >= g_histEdge) return false;
    if (t < g_histRowsFirst || t >= g_histRowsFirst + g_histRows.size()) {
        auto it = g_histIn.upper_bound(t);
        if (it == g_histIn.begin()) return false;
        --it;
        if (t >= it->first + it->second.count) return false;
        g_histRows.resize(it->second.count);
        if (!DecodeRows(it->second.data.data(), it->second.data.size(), it->second.count, g_players, g_histRows.data())) {
            g_histRows.clear();
            CatchUpFail(L"the match's record came damaged");
            return false;
        }
        g_histRowsFirst = it->first;
        g_histIn.erase(g_histIn.begin(), it);   // (the stretches before: played)
    }
    *out = g_histRows[t - g_histRowsFirst];
    return true;
}

// Ask the host for more of the record while it runs low (one answer at a time), and say where I am now and then.
void AskHistory() {
    const unsigned kAhead = 6000;   // ticks wanted ahead
    ULONGLONG now = GetTickCount64();
    static ULONGLONG lastAsk = 0, lastTell = 0;
    bool low = g_histEdge < g_tick + kAhead;
    if (low) {
        if (g_histAsked && now - lastAsk < 300) return;   // (an answer is on its way - or got lost)
        if (now - lastAsk < 10) return;
    } else if (now - lastTell < 2000) {
        return;
    }
    PktHistAsk q{{'J', (BYTE)g_slot}, g_histEdge, g_tick, (BYTE)(low ? kServeMax : 0)};
    SendAll(&q, sizeof(q));
    if (low) { lastAsk = now; g_histAsked = true; }
    lastTell = now;
}

// The host's word: I'm back in the match from tick `at`. My inputs from then: idle for the first few (so the
// others needn't wait for me while I catch up the last bit), then my own.
unsigned g_sampledUpTo = 0;          // my inputs are sampled up to this tick
void CatchUpBackAt(unsigned at) {
    g_myBackAt = at;
    unsigned upTo = at + (unsigned)g_delay - 1;
    for (unsigned t = at; t <= upTo; ++t) {
        int r = t % kRing;
        g_inputs[r][g_slot] = Pad{};
        g_have[r][g_slot] = true;
        g_haveTick[r][g_slot] = t;
    }
    g_sampledUpTo = upTo;
    g_paceMine = 0;   // (my pace while catching up said nothing: no 3D was drawn - measured again once I'm back)
    g_sentUpTo = at - 1;   // (nothing of mine before it: idle for everyone)
    SendInputs(upTo);
    Log(L"Netplay: catching up - back in the match from tick %u (here %u, the match at %u)", at, g_tick, g_hostTick);
}

void CatchUpDone() {
    g_catchUp = false;
    g_paceMine = 0;
    Log(L"Netplay: caught up with the match at tick %u, in %llu s - playing", g_tick,
        (GetTickCount64() - g_catchUpStartMs) / 1000);
    if (g_cuShared) InterlockedExchange(&g_cuShared->state, 1);
    HandoffCaughtUp();
}

// Everyone, at the tick the host's input says so: player s is back in the match a second from now.
void RejoinWord(int s) {
    if (s <= 0 || s >= g_players || s >= 4 || g_backAt[s]) return;
    g_backAt[s] = g_tick + (unsigned)g_rate + 2u * (unsigned)g_delay;
    g_goneAgain[s] = 0;
    Log(L"Netplay: tick %u - player %d rejoins the match: back from tick %u", g_tick, s, g_backAt[s]);
    if (s == g_slot && g_catchUp) {
        if (g_tick != g_myWordTick) {   // (an earlier rejoin of my slot, in the record: not this one)
            g_backAt[s] = 0;
            Log(L"Netplay: catching up - tick %u: an earlier rejoin of my slot", g_tick);
            return;
        }
        CatchUpBackAt(g_backAt[s]);
    }
}

void BackIn(unsigned tick) {
    for (int s = 0; s < g_players && s < 4; ++s) {
        if (!g_backAt[s] || tick < g_backAt[s]) continue;
        g_backAt[s] = 0;
        g_gone[s] = false;
        g_goneFrom[s] = g_goneAgain[s];
        g_goneAgain[s] = 0;
        g_rejoiners[s] = Rejoiner{};
        Log(L"Netplay: player %d is back in the match from tick %u", s, tick);
        if (g_inSession && s != g_slot && !g_catchUp) OverlayShow((PlayerName(s) + L" is back in the match").c_str(), 4000);
    }
}

// ---- direct connections ----------------------------------------------------------------------------------------
bool g_directWanted = false;         // mine on ([Online] DirectConnection; the online settings change it mid-match)

// The server: player `who` would like a direct connection too, and is at this address (as it sees them).
void DirectPeerAt(const PktPeerAt* p) {
    int who = p->who;
    if (!g_directWanted || who < 0 || who >= g_players || who >= 4 || who == g_slot || g_direct[who].ok) return;
    Direct& d = g_direct[who];
    sockaddr_in a{};
    a.sin_family = AF_INET;
    memcpy(&a.sin_addr, p->ip, 4);
    memcpy(&a.sin_port, p->port, 2);
    if (d.known && d.addr.sin_addr.s_addr == a.sin_addr.s_addr && d.addr.sin_port == a.sin_port) return;
    d.addr = a;
    d.known = true;
    d.punchUntil = GetTickCount64() + 20000;
    d.nextPunch = 0;
    Log(L"Netplay: player %d would like a direct connection too - trying it", who);
}

// Their 'Y', straight from them: answer it; once they've had mine (ack), everything goes to them straight too.
void DirectHello(int who, const sockaddr_in& from, bool ack) {
    if (!g_directWanted || who >= 4) return;
    Direct& d = g_direct[who];
    d.addr = from;   // (where their packets really come from: what to send to)
    d.known = true;
    char buf[64];
    buf[0] = 'F';
    buf[1] = 'G';
    memcpy(buf + 2, g_token, 8);
    buf[10] = (char)g_slot;
    PktDirect y{{'Y', (BYTE)g_slot}, 1};
    memcpy(buf + 11, &y, sizeof(y));
    if (!ack || !d.ok) sendto(g_sock, buf, 11 + (int)sizeof(y), 0, (const sockaddr*)&d.addr, sizeof(d.addr));
    if (ack && !d.ok) {
        d.ok = true;
        Log(L"Netplay: direct connection to player %d - packets go straight to them now (and through the server)", who);
    }
}

// Every tick: ask the server for the others' places now and then (while one isn't connected), and knock on theirs.
void DirectTick() {
    if (!g_relay || !g_inSession) return;
    bool want = OnlineDirectConnection();
    if (want != g_directWanted) {
        g_directWanted = want;
        if (!want) for (auto& d : g_direct) d = Direct{};
        Log(L"Netplay: direct connections %s", want ? L"on (with players who have them on too)" : L"off");
    }
    if (!g_directWanted) return;
    ULONGLONG now = GetTickCount64();
    static ULONGLONG nextAsk = 0;
    bool all = true;
    for (int s = 0; s < g_players && s < 4; ++s)
        if (s != g_slot && !Idle(s, g_tick) && !g_direct[s].ok) all = false;
    if (!all && now >= nextAsk) {
        nextAsk = now + 2000;
        PktHeader a{'a', (BYTE)g_slot};
        SendAll(&a, sizeof(a));
    }
    for (int s = 0; s < g_players && s < 4; ++s) {
        Direct& d = g_direct[s];
        if (s == g_slot || !d.known || d.ok || now > d.punchUntil || now < d.nextPunch) continue;
        d.nextPunch = now + 100;
        char buf[64];
        buf[0] = 'F';
        buf[1] = 'G';
        memcpy(buf + 2, g_token, 8);
        buf[10] = (char)g_slot;
        PktDirect y{{'Y', (BYTE)g_slot}, 0};
        memcpy(buf + 11, &y, sizeof(y));
        sendto(g_sock, buf, 11 + (int)sizeof(y), 0, (const sockaddr*)&d.addr, sizeof(d.addr));
    }
}

// A player asks for my inputs again from `from` on (one of my packets with them got lost): what I have, up to 64 ticks.
void ResendFrom(unsigned from) {
    if (from > g_sampledUpTo || g_sampledUpTo - from > 1024) return;
    static ULONGLONG lastAt = 0;
    static unsigned lastFrom = ~0u;
    ULONGLONG now = GetTickCount64();
    if (from == lastFrom && now - lastAt < 30) return;   // (asked again and again while it's on its way)
    lastAt = now;
    lastFrom = from;
    unsigned upTo = g_sampledUpTo - from > 63 ? from + 63 : g_sampledUpTo;
    for (unsigned first = from; first <= upTo;) {
        unsigned last = upTo - first > 15 ? first + 15 : upTo;
        PktInputs pk{};
        pk.h = {'I', (BYTE)g_slot};
        pk.first = first;
        pk.count = (BYTE)(last - first + 1);
        for (unsigned f = first; f <= last; ++f) pk.pads[f - first] = g_inputs[f % kRing][g_slot];
        SendAll(&pk, (int)offsetof(PktInputs, pads) + pk.count * (int)sizeof(Pad));
        first = last + 1;
    }
}

// Player `by` gave up waiting for player `who` and plays on without them from tick `from`: everyone does the same - and
// if that's me, my match is over (my game goes back to the lobby: the match can be rejoined from there).
volatile LONG g_droppedMe = 0;
void Dropped(int by, int who, unsigned from) {
    if (who < 0 || who >= g_players || who >= 4 || by == who) return;
    if (who == g_slot) {
        if (!InterlockedExchange(&g_droppedMe, 1))
            Log(L"Netplay: player %d stopped waiting for me at tick %u (my connection dropped) - back to the lobby", by, from);
        return;
    }
    if (g_backAt[who] || g_gone[who] || (g_goneFrom[who] && g_goneFrom[who] <= from)) return;
    if (from > g_tick) {
        g_goneFrom[who] = from;
        Log(L"Netplay: player %d dropped player %d from tick %u - so do I", by, who, from);
    } else {   // (past it here already: from now on)
        g_gone[who] = true;
        Log(L"Netplay: player %d dropped player %d from tick %u (here %u) - so do I", by, who, from, g_tick);
    }
}

void FreeInputs(int s, const PktInputs* p, int count);   // the free-running match (below, before Tick)
void FreeBack(int s);
struct PktPickup;
void FreePickupIn(int s, const PktPickup* p);
extern const int kPktPickupSize;
struct PktBuy;
void FreeBuyIn(int s, const PktBuy* p);
extern const int kPktBuySize;
struct PktState;
extern const int kPktStateSizeOld, kPktStateSizeMoney;
struct PktHit;
struct PktStats;
void FreeStatsIn(int s, const PktStats* p);
struct PktSpawn;
void FreeSpawnIn(int s, const PktSpawn* p);
extern const int kPktSpawnSize;
struct PktNpcStates;
void FreeNpcStatesIn(int s, const PktNpcStates* p, int n);
extern const int kPktNpcStatesHead;
struct PktNpcHit;
void FreeNpcHitIn(int s, const PktNpcHit* p);
extern const int kPktNpcHitSize;
struct PktNpcDown;
void FreeNpcDownIn(int s, const PktNpcDown* p);
extern const int kPktNpcDownSize;
extern const int kPktStatsSize;
void FreeState(int s, const PktState* p, bool withMoney, bool withClock);
void FreeHitIn(int s, const PktHit* p, bool withCredit);
extern const int kPktStateSize, kPktHitSize, kPktHitSizeOld;

void Receive() {
    char buf[1500];
    EnterCriticalSection(&g_netLock);
    struct Unlock { ~Unlock() { LeaveCriticalSection(&g_netLock); } } unlock;
    for (;;) {
        sockaddr_in from{};
        int fromLen = sizeof(from);
        int n = recvfrom(g_sock, buf, sizeof(buf), 0, (sockaddr*)&from, &fromLen);
        if (n == SOCKET_ERROR && WSAGetLastError() == WSAECONNRESET) continue;  // ICMP noise, not an error
        if (n == SOCKET_ERROR) return;  // nothing more to read
        bool straight = false;   // (not through the server: straight from a player)
        if (g_relay) {  // only our session's packets, through the server (or straight from a player in it)
            if (n < 11 + (int)sizeof(PktHeader) || buf[0] != 'F' || buf[1] != 'G' || memcmp(buf + 2, g_token, 8) != 0)
                continue;
            straight = !g_peers.empty() && (from.sin_addr.s_addr != g_peers[0].sin_addr.s_addr ||
                                            from.sin_port != g_peers[0].sin_port);
            memmove(buf, buf + 11, n - 11);
            n -= 11;
            if ((BYTE)buf[1] == 0xFE && !straight) {   // the server's own word: where a player is (direct connections)
                if (buf[0] == 'A' && n >= (int)sizeof(PktPeerAt)) DirectPeerAt(reinterpret_cast<PktPeerAt*>(buf));
                continue;
            }
        }
        if (n < (int)sizeof(PktHeader)) continue;
        auto h = reinterpret_cast<PktHeader*>(buf);
        int slot = h->slot;
        if (slot >= g_players || slot == g_slot) continue;  // not a peer's (our own inputs are never replaced)
        if (h->type == 'Y' && n >= (int)sizeof(PktDirect)) {   // straight from them: the direct connection works that way
            if (straight) DirectHello(slot, from, reinterpret_cast<PktDirect*>(buf)->ack != 0);
        } else if (h->type == 'R') {
            if (!g_peerReady[slot]) Log(L"Netplay: peer with slot %d is ready", slot);
            g_peerReady[slot] = true;
        } else if (h->type == 'I' && n >= (int)offsetof(PktInputs, pads)) {
            auto p = reinterpret_cast<PktInputs*>(buf);
            int count = (n - (int)offsetof(PktInputs, pads)) / (int)sizeof(Pad);  // what actually arrived
            if (count > p->count) count = p->count;
            if (g_free) { FreeInputs(slot, p, count); continue; }
            for (int i = 0; i < count && i < 16; ++i) {
                unsigned f = p->first + i;
                if (f + kRing / 2 < g_tick || f > g_tick + kRing / 2) continue;  // far too old / too far ahead
                int r = f % kRing;
                g_inputs[r][slot] = p->pads[i];
                g_have[r][slot] = true;
                g_haveTick[r][slot] = f;
            }
        } else if (h->type == 'H' && n >= (int)sizeof(PktHash)) {
            if (g_free) continue;   // (sync checks: lockstep only - a rejoining game sends them while it loads)
            auto p = reinterpret_cast<PktHash*>(buf);
            int k = (p->tick / g_syncEvery) % 8;
            g_peerHash[slot][k] = p->hash;
            g_peerPlayers[slot][k] = p->players;
            g_peerRng[slot][k] = p->rng;
            g_peerHashTick[slot][k] = p->tick;
            if (!g_gone[slot] && !g_goneFrom[slot] && !g_backAt[slot])   // (not from a player who isn't playing)
                CompareHash(slot, p->tick, p->hash, p->players, p->rng);
        } else if (h->type == 'M' && n >= (int)sizeof(PktRng)) {
            RngReceive(reinterpret_cast<PktRng*>(buf));
        } else if (h->type == 'Q' && n >= (int)sizeof(PktRngAsk)) {
            RngAsked(reinterpret_cast<PktRngAsk*>(buf)->tick);
        } else if (h->type == 'N' && n >= (int)sizeof(PktResend)) {   // my inputs again, from that tick
            auto p = reinterpret_cast<PktResend*>(buf);
            if (p->to == g_slot) ResendFrom(p->from);
        } else if (h->type == 'D' && n >= (int)sizeof(PktDropped)) {
            auto p = reinterpret_cast<PktDropped*>(buf);
            Dropped(slot, p->who, p->from);
        } else if (h->type == 'B') {   // a player rejoined a free-running match (a new game: their numbers start over)
            if (g_free) FreeBack(slot);
        } else if (h->type == 'U' && n >= kPktPickupSize) {   // a pickup they took
            if (g_free) FreePickupIn(slot, reinterpret_cast<PktPickup*>(buf));
        } else if (h->type == 'E' && n >= kPktSpawnSize) {   // an enemy the host's game spawned
            if (g_free) FreeSpawnIn(slot, reinterpret_cast<PktSpawn*>(buf));
        } else if (h->type == 'G' && n >= kPktNpcStatesHead) {   // the host's enemies: where, how hurt
            if (g_free) FreeNpcStatesIn(slot, reinterpret_cast<PktNpcStates*>(buf), n);
        } else if (h->type == 'V' && n >= kPktNpcHitSize) {   // a player's hit on an enemy, for the host's game
            if (g_free) FreeNpcHitIn(slot, reinterpret_cast<PktNpcHit*>(buf));
        } else if (h->type == 'K' && n >= kPktNpcDownSize) {   // an enemy the host's game has down
            if (g_free) FreeNpcDownIn(slot, reinterpret_cast<PktNpcDown*>(buf));
        } else if (h->type == 'T' && n >= kPktStatsSize) {   // the host's scoreboard
            if (g_free) FreeStatsIn(slot, reinterpret_cast<PktStats*>(buf));
        } else if (h->type == 'W' && n >= kPktBuySize) {   // a weapon they bought at a supply crate
            if (g_free) FreeBuyIn(slot, reinterpret_cast<PktBuy*>(buf));
        } else if (h->type == 'X' && n >= kPktStateSizeOld) {   // (0.8.12 and older: no money in it)
            if (g_free) FreeState(slot, reinterpret_cast<PktState*>(buf), n >= kPktStateSizeMoney, n >= kPktStateSize);
        } else if (h->type == 'Z' && n >= kPktHitSizeOld) {   // (0.8.13 and older: no credit in it)
            if (g_free) FreeHitIn(slot, reinterpret_cast<PktHit*>(buf), n >= kPktHitSize);
        } else if (h->type == 'L' && n >= (int)sizeof(PktLeave)) {
            auto p = reinterpret_cast<PktLeave*>(buf);
            if (g_free) {   // (free-running: their tick count isn't mine - they're simply out)
                if (!g_gone[slot]) Log(L"Netplay: free - player %d left the match (back to the lobby)", slot);
                g_gone[slot] = true;
            } else if (g_backAt[slot] && p->last + 1 >= g_backAt[slot]) {   // rejoining, and gone again from then
                if (!g_goneAgain[slot] || p->last + 1 < g_goneAgain[slot]) {
                    g_goneAgain[slot] = p->last + 1;
                    Log(L"Netplay: player %d left again - idle from tick %u", slot, p->last + 1);
                }
            } else if (!g_goneFrom[slot] || p->last + 1 < g_goneFrom[slot]) {
                g_goneFrom[slot] = p->last + 1;
                Log(L"Netplay: player %d left the match (back to the lobby) - idle from tick %u", slot, p->last + 1);
            }
        } else if (h->type == 'P' && n >= (int)sizeof(PktPing)) {   // a ping: echoed back to its sender
            auto p = reinterpret_cast<PktPing*>(buf);
            static unsigned long long echoed[4];   // (once: with a direct connection it comes twice)
            if (p->sent == echoed[slot]) continue;
            echoed[slot] = p->sent;
            PktPing echo{{'O', (BYTE)g_slot}, (BYTE)slot, p->sent};
            SendAll(&echo, sizeof(echo));
        } else if (h->type == 'O' && n >= (int)sizeof(PktPing)) {   // my ping's echo
            auto p = reinterpret_cast<PktPing*>(buf);
            static unsigned long long timed[4];   // (the first echo: the quicker way - the second comes the slower)
            if (p->to == g_slot && p->sent != timed[slot]) {
                timed[slot] = p->sent;
                ULONGLONG now = GetTickCount64();
                if (now >= p->sent && now - p->sent < 60000) {
                    InterlockedExchange(&g_pingMs[slot], (LONG)(now - p->sent));
                    g_pingRecent[slot][g_pingAt[slot]++ % kPingRecent] = (int)(now - p->sent);
                }
            }
        } else if (h->type == 'J' && n >= (int)sizeof(PktHistAsk)) {   // a player rejoining asks for the match so far
            if (g_slot == 0 && g_inSession) ServeHistory(slot, reinterpret_cast<PktHistAsk*>(buf));
        } else if (h->type == 'K' && n >= (int)sizeof(PktHist)) {          // the host's record, for me catching up
            if (g_catchUp && slot == 0) StoreHistory(reinterpret_cast<PktHist*>(buf), n);
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
    for (int s = 0; s < g_players; ++s) {
        bool idle = Idle(s, tick);   // (left the match, or not back yet: idle, whatever arrived)
        if (!idle && g_have[r][s] && g_haveTick[r][s] == tick) continue;
        if (!idle) return false;
        g_inputs[r][s] = Pad{};
        g_have[r][s] = true;
        g_haveTick[r][s] = tick;
    }
    return true;
}


void SendInputs(unsigned upTo) {
    g_lastSent = upTo;
    unsigned first = upTo >= 7 ? upTo - 7 : 0;  // resend the last 8 ticks (packet loss; ~27 KB/s per player)
    if (g_sentUpTo + 1 < first) first = g_sentUpTo + 1;   // and every tick not sent yet (the delay grew a lot at once)
    if (upTo > g_sentUpTo) g_sentUpTo = upTo;
    while (first <= upTo) {   // (16 ticks a packet)
        unsigned last = upTo - first > 15 ? first + 15 : upTo;
        PktInputs p{};
        p.h = {'I', (BYTE)g_slot};
        p.first = first;
        p.count = (BYTE)(last - first + 1);
        for (unsigned f = first; f <= last; ++f) p.pads[f - first] = g_inputs[f % kRing][g_slot];
        SendAll(&p, (int)offsetof(PktInputs, pads) + p.count * (int)sizeof(Pad));
        first = last + 1;
    }
}

// ---- local input ----------------------------------------------------------------------------------------
using XInputGetState_t = DWORD(WINAPI*)(DWORD, XINPUT_STATE*);
XInputGetState_t g_realXInput = nullptr;
unsigned g_playStartTick = 0;        // test script: first tick with players in the level
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
// ---- the random numbers' safety net -------------------------------------------------------------------------
// The host sees the random numbers went apart (the players still match): its input a few ticks on says "take
// mine" (Pad.kbm.sync), so every PC reaches that tick with the same instruction. There the host sends its random
// numbers as they stand at the start of the tick, and every other PC waits for them and takes them over.
bool g_rngWanted = false;            // host: put the instruction in my next input
unsigned g_rngDriftTick = 0;         // host: the check that asked for it (once per check)
unsigned g_rngSnapTick = 0;          // host: the tick of the numbers sent (to send again if asked)
PktRng g_rngSnap[3];
unsigned g_rngWaitTick = 0;          // others: the tick we're waiting for the host's numbers at
bool g_rngHave[3];
PktRng g_rngIn[3];

void RngDrift(unsigned tick) {
    if (g_slot != 0 || !g_inSession || tick == g_rngDriftTick) return;
    g_rngDriftTick = tick;
    g_rngWanted = true;
    Log(L"Netplay: tick %u - the random numbers went apart (the players match) - everyone takes the host's", tick);
}

void RngSendSnap() {
    for (auto& p : g_rngSnap) SendAll(&p, sizeof(p));
}

void RngAsked(unsigned tick) {
    if (g_slot != 0) return;
    if (tick == g_rngSnapTick) { RngSendSnap(); return; }
    for (auto& p : g_rngAll) if (p.tick == tick) SendAll(&p, sizeof(p));   // (a player catching up)
}

void RngReceive(const PktRng* p) {
    if (g_slot == 0 || p->tick != g_rngWaitTick || p->part > 2) return;
    g_rngIn[p->part] = *p;
    g_rngHave[p->part] = true;
}

// At the start of a tick whose host input says so (every PC, the same tick).
void RngResync() {
    unsigned* mt = reinterpret_cast<unsigned*>(0xb09360);
    unsigned& next = *reinterpret_cast<unsigned*>(0xb09358);
    int& left = *reinterpret_cast<int*>(0xada67c);
    if (g_slot == 0) {
        for (int part = 0; part < 3; ++part) {
            PktRng& p = g_rngSnap[part];
            p.h = {'M', (BYTE)g_slot};
            p.tick = g_tick;
            p.part = (BYTE)part;
            p.next = (int)(next - 0xb09360u);
            p.left = left;
            memcpy(p.words, mt + part * kRngPart, sizeof(p.words));
        }
        g_rngSnapTick = g_tick;
        for (auto& part : g_rngSnap) g_rngAll.push_back(part);   // (a player who rejoins needs them too)
        RngSendSnap();
        Log(L"Netplay: tick %u - sent my random numbers to everyone", g_tick);
        return;
    }
    g_rngWaitTick = g_tick;
    ULONGLONG from = GetTickCount64(), nextAsk = from + 40;
    for (;;) {
        Receive();
        if (g_rngHave[0] && g_rngHave[1] && g_rngHave[2]) break;
        ULONGLONG now = GetTickCount64();
        if (now - from > 5000) {
            Log(L"Netplay: tick %u - the host's random numbers didn't come", g_tick);
            g_rngWaitTick = 0;
            if (g_catchUp) CatchUpFail(L"the host's random numbers didn't come");
            return;
        }
        if (now >= nextAsk) {
            PktRngAsk q{{'Q', (BYTE)g_slot}, g_tick};
            SendAll(&q, sizeof(q));
            nextAsk = now + 40;
        }
        Real_Sleep(1);
    }
    for (int part = 0; part < 3; ++part) memcpy(mt + part * kRngPart, g_rngIn[part].words, sizeof(g_rngIn[part].words));
    next = 0xb09360u + (unsigned)g_rngIn[0].next;
    left = g_rngIn[0].left;
    g_rngHave[0] = g_rngHave[1] = g_rngHave[2] = false;
    g_rngWaitTick = 0;
    Log(L"Netplay: tick %u - took the host's random numbers (waited %llu ms)", g_tick, GetTickCount64() - from);
}

void NoteMismatch(unsigned tick) {
    static unsigned lastBadTick = 0;
    static int badRun = 0;
    if (!g_inSession || !g_liveTick || tick <= g_liveTick + 600 || tick == lastBadTick) return;
    badRun = lastBadTick && tick - lastBadTick <= (unsigned)g_syncEvery ? badRun + 1 : 1;
    lastBadTick = tick;
    if (badRun == 3) {
        if (g_catchUp) { CatchUpFail(L"the match came out differently here"); return; }
        Log(L"Netplay: out of sync since tick %u - back to the lobby", tick - 2 * g_syncEvery);
        OverlayShow(L"The match went out of sync - back to the lobby", 6000);
        ReportDesync(tick - 2 * g_syncEvery, "players parted");
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

// Host: what everyone must do at the same tick goes in my input for tick f (Pad.kbm.sync): 1 take my random numbers,
// 0x10 | player: that player has caught up - back in the match a second on (RejoinWord).
void HostWord(Pad* p, unsigned f) {
    if (g_slot != 0 || !g_inSession) return;
    if (g_rngWanted) {
        p->kbm.sync = 1;
        g_rngWanted = false;
        Log(L"Netplay: everyone takes my random numbers at tick %u", f);
        return;
    }
    ULONGLONG now = GetTickCount64();
    unsigned closeBy = (unsigned)(g_rate / 2 > 3 * g_delay ? g_rate / 2 : 3 * g_delay);
    for (int s = 1; s < g_players && s < 4; ++s) {
        Rejoiner& r = g_rejoiners[s];
        if (!r.heardAt || now - r.heardAt > 1000 || g_backAt[s] || !Idle(s, f) || g_tick > r.at + closeBy ||
            (r.issued && f < r.issued + 2u * (unsigned)g_rate))
            continue;
        p->kbm.sync = (BYTE)(0x10 | s);
        r.issued = f;
        Log(L"Netplay: player %d has nearly caught up (at tick %u, here %u) - back in the match, from my input for tick %u",
            s, r.at, g_tick, f);
        return;
    }
}

// The player's own controller: the first one Windows has plugged in (XInput slots 0..3 - not always slot 0: a tester's
// was elsewhere, and their match read nothing; the lobby's "auto" looks at all four too). Looked for again every 2 s
// while there's none, and when the one in use goes.
DWORD MyPadState(XINPUT_STATE* st) {
    static int at = -1;
    static ULONGLONG nextLook = 0;
    if (!g_realXInput) return ERROR_DEVICE_NOT_CONNECTED;
    if (at >= 0 && g_realXInput((DWORD)at, st) == ERROR_SUCCESS) return ERROR_SUCCESS;
    ULONGLONG now = GetTickCount64();
    if (at >= 0 || now >= nextLook) {
        nextLook = now + 2000;
        int was = at;
        at = -1;
        for (DWORD i = 0; i < 4 && at < 0; ++i)
            if (g_realXInput(i, st) == ERROR_SUCCESS) at = (int)i;
        if (at != was) Log(L"Netplay: my controller: %s", at < 0 ? L"none" : at == 0 ? L"slot 1" : at == 1 ? L"slot 2" : at == 2 ? L"slot 3" : L"slot 4");
        if (at >= 0) return ERROR_SUCCESS;
    }
    ZeroMemory(st, sizeof(*st));
    return ERROR_DEVICE_NOT_CONNECTED;
}

Pad SampleLocal(unsigned f) {
    if (g_script) {
        Pad sp = ScriptPad(f);
        if (HandoffHudUp()) sp.kbm.flags |= 1;   // my level's HUD is up (as below)
        if (g_inSession) sp.kbm.pace = (BYTE)g_paceMine;
        if (g_inSession && OnlinePauseBotVotes()) sp.kbm.flags |= 16;   // (the rematch vote, onlinepause.cpp)
        HostWord(&sp, f);
        return sp;
    }
    Pad p{};
    XINPUT_STATE st{};
    if (MyPadState(&st) == ERROR_SUCCESS) {  // the player's own controller
        p.buttons = st.Gamepad.wButtons;
        p.lt = st.Gamepad.bLeftTrigger;
        p.rt = st.Gamepad.bRightTrigger;
        p.lx = st.Gamepad.sThumbLX;
        p.ly = st.Gamepad.sThumbLY;
        p.rx = st.Gamepad.sThumbRX;
        p.ry = st.Gamepad.sThumbRY;
    }
    p.buttons |= VPadButtons(0);  // test tools press this player's buttons through the pipe
    XINPUT_GAMEPAD held{};
    if (VPadHeld(0, &held)) {     // (and hold sticks / triggers: "hold")
        p.buttons |= held.wButtons;
        p.lx = held.sThumbLX; p.ly = held.sThumbLY; p.rx = held.sThumbRX; p.ry = held.sThumbRY;
        p.lt = held.bLeftTrigger; p.rt = held.bRightTrigger;
    }
    if (KbmIsKeyboardSlot(g_slot)) KbmCapture(&p.kbm);  // keyboard / mouse players: the raw state (kbm.cpp)
    if (HandoffHudUp()) p.kbm.flags |= 1;              // my level's HUD is up (kbm.cpp waits for everyone's)
    if (g_inSession && OnlinePauseSample(&p.buttons, &p.lx, &p.ly, &p.kbm, KbmIsKeyboardSlot(g_slot))) {
        p.lt = p.rt = 0;   // my pause menu is open: my character stands still (onlinepause.cpp)
        p.lx = p.rx = p.ry = 0;
    }
    if (g_inSession) OnlineSettingsStamp(&p.kbm);   // my aim settings go with my input (onlinesettings.cpp)
    if (g_inSession) p.kbm.pace = (BYTE)g_paceMine;   // and how many frames a second this PC manages
    HostWord(&p, f);   // (the random numbers' safety net, a player back in the match)
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

// The match's players only: the game also keeps spare player objects (index >= the player count), whose aim holds
// leftovers that differ from PC to PC - they're in no one's match.
unsigned HashPlayers(Hashed* keep) {
    unsigned h = 2166136261u;
    unsigned char* all[8];
    unsigned char* players[8];
    int found = GamePlayers(all, 8), n = 0;
    for (int i = 0; i < found; ++i)
        if (*reinterpret_cast<int*>(all[i] + 0xdf8) < g_players) players[n++] = all[i];
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
        // (an aim that isn't a place in a level - leftover memory between a match and the menus: millions - is
        // left out: it made false "out of sync" alarms)
        const float* a = reinterpret_cast<const float*>(aim);
        if (aim && (std::fabs(a[0]) > 1e5f || std::fabs(a[1]) > 1e5f || std::fabs(a[2]) > 1e5f || a[0] != a[0] ||
                    a[1] != a[1] || a[2] != a[2]))
            aim = nullptr;
        for (int k = 0; aim && k < 12; ++k) h = (h ^ aim[k]) * 16777619u;
        // and their money (the wallet: player +0xbc, its +0x224 - what the store's prices are checked against; a
        // purchase that went through on one PC only went unnoticed until the random numbers parted)
        int money = PlayerMoney(self);
        keep->money[i] = money;
        for (int k = 0; k < 4; ++k) h = (h ^ ((money >> (k * 8)) & 0xff)) * 16777619u;
        // and their health (a player who died on one PC only: Multiverse Madness was over there - everyone down -
        // and went on everywhere else)
        float health = PlayerHealth(self);
        keep->health[i] = health;
        unsigned hb;
        memcpy(&hb, &health, 4);
        for (int k = 0; k < 4; ++k) h = (h ^ ((hb >> (k * 8)) & 0xff)) * 16777619u;
    }
    // The game's random numbers (one Mersenne Twister, rngtrace.cpp: 624 words at 0xb09360, the next one's address
    // at 0xb09358, words left at 0xada67c): every PC must have drawn the same ones. Hashed on its own as well, so a
    // mismatch says whether the players or the random numbers went apart first.
    const unsigned* mt = reinterpret_cast<const unsigned*>(0xb09360);
    unsigned r = 2166136261u;
    for (int i = 0; i < 624; ++i) r = (r ^ mt[i]) * 16777619u;
    int left = *reinterpret_cast<const int*>(0xada67c);
    unsigned next = *reinterpret_cast<const unsigned*>(0xb09358) - 0xb09360u;
    r = ((r ^ (unsigned)left) * 16777619u ^ next) * 16777619u;
    keep->players = h;
    keep->rng = r;
    keep->rngLeft = left;
    return (h ^ r) * 16777619u;
}

int PlayerMoney(const unsigned char* player) {   // (the wallet, 0x5bf420: player +0xbc, its +0x224)
    const int* money = reinterpret_cast<const int*>(player + 0xbc + 0x224);
    return IsBadReadPtr(money, 4) ? 0 : *money;
}

float PlayerHealth(const unsigned char* player) {   // (player +0x5c points to its health: +0x140, 200 when full)
    auto obj = *reinterpret_cast<const unsigned char* const*>(player + 0x5c);
    const float* health = obj ? reinterpret_cast<const float*>(obj + 0x140) : nullptr;
    return !health || IsBadReadPtr(health, 4) ? 0.0f : *health;
}

void LogPlayers(const wchar_t* tag) {
    unsigned char* players[8];
    int n = GamePlayers(players, 8);
    for (int i = 0; i < n; ++i) {
        const float* pos = PlayerPos(players[i]);
        const float* aim = PlayerAim(players[i]);
        static const float none[3] = {0, 0, 0};
        if (!aim) aim = none;
        Log(L"Netplay: %s tick %u player %d at (%.4f, %.4f, %.4f) aim (%.3f, %.3f, %.3f) money %d health %.3f", tag, g_tick,
            *reinterpret_cast<int*>(players[i] + 0xdf8), pos[0], pos[1], pos[2], aim[0], aim[1], aim[2],
            PlayerMoney(players[i]), PlayerHealth(players[i]));
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
bool g_sessionDM = false;        // a Deathmatch session (the only mode the free-running match is for, so far)
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
    if (g_catchUp) {   // rejoining: everyone's long past it - the first tick is the host's record's
        g_phase = Phase::Running;
        CloseHandle(CreateThread(nullptr, 0, Watchdog, nullptr, 0, nullptr));
        g_catchUpStartMs = g_histHeardAt = GetTickCount64();
        Log(L"Netplay: rejoining the match - playing it from the first tick on the host's record, as fast as this PC can "
            L"(slot %d of %d players, input delay %d)", g_slot, g_players, g_delay);
        return;
    }
    if (g_freeRejoin) {   // rejoining a free-running match: on my own until my level is up (FreeStart)
        for (int s = 0; s < g_players && s < 4; ++s) if (s != g_slot) g_gone[s] = true;
        for (unsigned f = 0; f < (unsigned)g_delay; ++f) {
            g_inputs[f % kRing][g_slot] = Pad{};
            g_have[f % kRing][g_slot] = true;
            g_haveTick[f % kRing][g_slot] = f;
        }
        g_phase = Phase::Running;
        CloseHandle(CreateThread(nullptr, 0, Watchdog, nullptr, 0, nullptr));
        Log(L"Netplay: rejoining a free-running match - loading the level on my own first (slot %d of %d players)", g_slot,
            g_players);
        return;
    }
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
        if (g_catchUp) {
            if ((now - start) % 400 > 200) buttons = XINPUT_GAMEPAD_A;   // catching up: skipped right away (nobody sees it)
        } else if (g_script) {
            if (g_slot == 0 && (now - start) % 2000 > 1500) buttons = XINPUT_GAMEPAD_A;  // tap every 2 s
        } else if (g_realXInput) {
            XINPUT_STATE st{};
            if (MyPadState(&st) == ERROR_SUCCESS) buttons = st.Gamepad.wButtons;
            buttons |= VPadButtons(0);
        }
        if (!g_catchUp && KbmSkipPressed()) buttons |= XINPUT_GAMEPAD_A;  // Space / Enter / Esc skip too
        WORD pressed = buttons & ~g_localButtonsLast;
        g_localButtonsLast = buttons;
        if (pressed & (XINPUT_GAMEPAD_A | XINPUT_GAMEPAD_START) && now >= g_skipPressUntil + 300) {
            ++g_skipSeq;
            if (!g_catchUp) g_skipSendUntil = now + 300;   // (catching up: my scene only)
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

// Real time keeps step with the fixed clock: each tick starts no sooner than its time step after the last.
// (Here rather than at Present: a paused game, or one whose window can't be seen, stops presenting, and
// then ran the match several times too fast.) Not while an online session boots hidden: that runs flat out.
// Returns how long it waited (QPC counts) - not work this PC had to do.
LONGLONG PaceTick() {
    static LONGLONG next = 0;
    if (g_phase != Phase::Running || HandoffBooting()) {
        next = 0;
        return 0;
    }
    LARGE_INTEGER now, from;
    Real_QPC(&now);
    from = now;
    LONGLONG step = g_freq.QuadPart / g_rate;
    if (!next) next = now.QuadPart;
    if (now.QuadPart < next) {
        LONGLONG ms = (next - now.QuadPart) * 1000 / g_freq.QuadPart;
        if (ms > 1) Real_Sleep((DWORD)(ms - 1));
        do Real_QPC(&now); while (now.QuadPart < next);
    }
    next += step;
    if (now.QuadPart - next > g_freq.QuadPart / 4) next = now.QuadPart;  // far behind: don't race to catch up
    return now.QuadPart - from.QuadPart;
}

// The frame rates a match steps through (each PC says the highest of these it manages, with some room spare).
const int kPaceSteps[] = {240, 200, 165, 144, 120, 100, 90, 80, 72, 60, 50, 45, 40, 36, 30};

// This PC's pace from its last stretch of ticks: `perSecond` = ticks it could do a second, by its own work.
// Down at once, straight to the rate it manages right now (stepping down through the rates, a window each, ran the
// match faster than the PC could for a couple of seconds: slow motion). Up only one step at a time, with room spare,
// after a second at the same rate - and not back to a rate it just couldn't hold (for a minute: each try that
// fails is a short slow-down): a slower rate leaves the graphics card idle time, so the PC looks quicker there
// than it is at the higher rate (it went 144 -> 100 -> 144 -> 100 ... before). The match starts following the PCs
// from the 60 it loaded at, and climbs (it started at the top before: a second or so of slow motion).
void NotePace(double perSecond) {
    static double smooth = 0;
    static ULONGLONG since = 0, ceilingUntil = 0;
    static int ceiling = 0;
    if (!g_rateFollows || g_catchUp) return;   // (loading the level / catching up says nothing about the match)
    smooth = smooth ? smooth * 0.5 + perSecond * 0.5 : perSecond;
    ULONGLONG nowMs = GetTickCount64();
    int cap = g_rateTarget, now = g_paceMine ? g_paceMine : 60, want = now;
    if (perSecond * 0.93 < now) {   // can't keep up: down to what it manages now
        want = kPaceSteps[_countof(kPaceSteps) - 1];
        for (int s : kPaceSteps)
            if (s <= cap && perSecond * 0.93 >= s) { want = s; break; }
    } else {                        // room spare: the next step up, now and then
        int next = now;
        for (int s : kPaceSteps)
            if (s <= cap && s > now) next = s;
        bool held = nowMs >= since + 1000, blocked = nowMs < ceilingUntil && next >= ceiling;
        if (next > now && held && !blocked && smooth * 0.85 >= next) want = next;
    }
    if (want > cap) want = cap;
    if (want == now && g_paceMine) return;
    if (want < now && g_paceMine) {   // this rate didn't hold: don't come back to it for a while
        ceiling = now;
        ceilingUntil = nowMs + 60000;
    }
    static int logged = 0;
    if (g_inSession && ++logged <= 40)
        Log(L"Netplay: tick %u - this PC manages %d frames per second (%.0f with no room spare)", g_tick, want, perSecond);
    g_paceMine = want;
    since = nowMs;
}

// ---- catching up, every tick ---------------------------------------------------------------------------------------
// This tick's inputs: the host's record of it (true), or - once I'm back in the match - everyone's as they come
// (false: in the ring), whichever is here first. Waits for them.
bool CatchUpInputs(Row* row) {
    ULONGLONG from = GetTickCount64();
    for (;;) {
        Receive();
        if (g_catchUpFailed) { *row = Row{}; return true; }   // (given up: the game runs on, idle, until it quits)
        if (HistRow(g_tick, row)) return true;
        if (g_catchUpFailed) { *row = Row{}; return true; }
        if (g_myBackAt && g_tick >= g_myBackAt && HaveAll(g_tick)) return false;
        AskHistory();
        ULONGLONG now = GetTickCount64(), heard = g_histHeardAt > from ? g_histHeardAt : from;
        if (now - heard > 20000)
            CatchUpFail(g_hostTick ? L"the match stopped answering" : L"the match didn't answer");
        Real_Sleep(1);
    }
}

// How far it is (my lobby game's loading screen), and giving up if it can't get there.
void CatchUpProgress() {
    if (g_cuShared) {
        InterlockedExchange(&g_cuShared->tick, (LONG)g_tick);
        InterlockedExchange(&g_cuShared->hostTick, (LONG)(g_hostTick > g_tick ? g_hostTick : g_tick));
    }
    static ULONGLONG checkAt = 0;
    static long lastBehind = -1;
    static int worse = 0;
    ULONGLONG now = GetTickCount64();
    if (now - g_catchUpStartMs > 40 * 60000) { CatchUpFail(L"it took too long"); return; }
    if (!checkAt) checkAt = now + 60000;
    if (now < checkAt || !g_hostTick) return;
    checkAt = now + 60000;   // every minute: am I any closer? (twice not: this PC is too slow to catch up)
    long behind = (long)g_hostTick - (long)g_tick;
    if (lastBehind >= 0 && behind >= lastBehind) {
        if (++worse >= 2) CatchUpFail(L"this PC can't catch up with it");
    } else {
        worse = 0;
    }
    Log(L"Netplay: catching up - tick %u, the match at %u", g_tick, g_hostTick);
    lastBehind = behind;
}

// Catching up: my sync check against the host's at the same tick - it must come out the same.
void CheckHostHash(unsigned tick, const Hashed& mine) {
    auto it = g_histHashes.find(tick);
    if (it == g_histHashes.end()) return;
    HashRec host = it->second;
    g_histHashes.erase(g_histHashes.begin(), std::next(it));
    if (host.players != mine.players) {
        Log(L"Netplay: catching up - tick %u came out differently from the host's (players %08x, the host's %08x)", tick,
            mine.players, host.players);
        NoteMismatch(tick);
    } else if (host.rng != mine.rng) {
        static int logged = 0;
        if (logged++ < 5) Log(L"Netplay: catching up - tick %u: the players as the host's, the random numbers not", tick);
    } else if (tick % (g_syncEvery * 20) == 0) {
        Log(L"Netplay: catching up - tick %u as the host had it", tick);
    }
}

// ---- my view turns at once (display only) ----------------------------------------------------------------------
// The frame being drawn now shows the match with this tick's input taken in already; my inputs after it, up to the
// last one sampled, haven't turned the camera yet. How far they will (radians, the camera's own sense), for splitview.cpp to
// draw my view turned that much already. Measured in the game (keyboard / mouse; sensitivity 1): 0.1069 degrees a
// mouse count up to 20 counts a tick, a little more per count above that (0.1203 at 40, 0.1337 at 60), and the game
// takes 60 a tick at most; times the player's sensitivity (0xbad810, by list index). The camera never looks up or
// down. Controllers: not yet (their turn speeds up the longer the stick is held).
float MouseTurnDegrees(int counts) {
    int a = counts < 0 ? -counts : counts;
    if (a > 60) a = 60;
    float per = 0.1069f + (a > 20 ? 0.000668f * (a - 20) : 0.0f);
    return (counts < 0 ? -1.0f : 1.0f) * a * per;
}

// The model's turn of each tick's input of mine (radians, by tick in the ring), and the tick the frame being drawn
// shows: splitview.cpp compares the game's camera with them, frame by frame, to learn how far off the model is (the
// game's turn per count isn't the same at every frame rate).
float g_turnByTick[kRing];
volatile LONG g_shownTick = 0;

void PredictMyView() {
    bool on = !g_catchUp && NetplayLevelLive() && KbmIsKeyboardSlot(g_slot) && !OnlinePauseMenusUp();
    float turn = 0, x = 0, z = 0;
    // (the frame drawn meanwhile has this tick's input in it already - measured: one tick's turn too far otherwise)
    InterlockedExchange(&g_shownTick, (LONG)g_tick);
    for (unsigned t = g_tick + 1; t <= g_sampledUpTo && t - g_tick < 512; ++t) g_turnByTick[t % kRing] = 0;   // (below, if on)
    if (on) {
        auto list = *reinterpret_cast<unsigned char* volatile*>(0xbad988);
        unsigned char* me = nullptr;
        float sensitivity = 0;
        int count = list && !IsBadReadPtr(list, 0x24) ? *reinterpret_cast<int*>(list + 0x20) : 0;
        for (int i = 0; i < count && i < 8; ++i) {
            auto p = reinterpret_cast<unsigned char**>(list)[i];
            if (p && LivePlayer(p) && *reinterpret_cast<int*>(p + 0xdf8) == g_slot) {
                me = p;
                sensitivity = reinterpret_cast<const float*>(0xbad810)[i];
            }
        }
        const float* pos = me ? PlayerPos(me) : nullptr;
        on = pos && PlayerHealth(me) > 0 && sensitivity > 0 && sensitivity <= 4;
        if (on) {
            for (unsigned t = g_tick + 1; t <= g_sampledUpTo && t - g_tick < 512; ++t) {
                // (the camera's angle goes down as the mouse goes right)
                float r = -MouseTurnDegrees(g_inputs[t % kRing][g_slot].kbm.dx) * sensitivity * 3.14159265f / 180.0f;
                g_turnByTick[t % kRing] = r;
                turn += r;
            }
            x = pos[0];
            z = pos[2];
        }
    }
    SplitViewPredict(on, turn, x, z);
}

// ---- free-running match (client-server, being built: FGUYMOD_NET_FREE=1 on every copy) ----------------------------
// The session boots, walks its menus and loads the level in lockstep as above. Two seconds after everyone's level is up
// (the same tick on every PC) every PC lets go: from then on each runs on its own - real clock, its own frame rate, no
// waiting for anyone. My input plays at once here and goes to the others every frame; their characters are driven by
// their latest input (a button tapped between two of my frames still counts) and pulled towards where their own PC has
// them ('X', 30 times a second: position, camera angles, health). Hits: the PC of the player who caused one decides it
// (a player's own PC for everything else) and sends it as the game made it ('Z'); every other PC plays it through the
// game's own TakeDamage, so hit reactions, knockback and deaths happen everywhere; the victim's own PC has the last word
// on their health. Research: RESEARCH_NOTES.md "Client-server research".
bool g_freeWanted = false;
bool g_free = false;
unsigned g_freeFrom = 0;
bool g_freeHostGone = false;                   // the host left: back to the lobby (once)
// Rejoining a free-running match (FGUYMOD_NET_REJOIN, online.cpp): this game boots and loads the level on its own -
// everyone else idle in it, no waiting for them (they're far into the match) - and from 2 s after its level is up joins
// the match: 'B' tells the others I'm back (they start over with my numbers: this is a new game).
bool g_freeRejoin = false;
unsigned g_freeRejoinHud = 0;                  // the tick my level's HUD came up (rejoining)
ULONGLONG g_freeBackUntil = 0;                 // rejoined: say 'B' until then (packets get lost)
const unsigned kFreeAfterTicks = 120;          // after everyone's level is up (at the lockstep's 60)

struct Remote {
    unsigned lastTick;                         // their newest input's tick (their own count)
    Pad latest;
    WORD buttonsSince;                         // what went down since I last played their input
    BYTE keysSince[32], mouseSince;
    ULONGLONG heardAt;                         // their last packet
    ULONGLONG backAt;                          // when they rejoined (their 'B')
    bool haveState;
    unsigned stateSeq;
    float pos[3], yaw, pitch, health;
    bool alive;
    bool haveMoney;
    int money;                                 // their money (what the store's prices are checked against)
    float yawVel, pitchVel;                    // how fast they're turning (radians a second, from their last states)
    double stateAt;                            // when their last state came (PreciseMs)
    unsigned stateSentMs;                      //   and their clock then (0: their PC doesn't say)
    unsigned hitAck[4];                        // the last hit from each PC their own PC had counted (their state)
    unsigned hitSeen[32];                      // their last hits' numbers (a hit comes twice)
    unsigned hitApplied;                       // the last of their hits I've played here
    unsigned pickupSeen[16];                   // their last pickups' numbers (each comes twice)
    unsigned buySeen[8];                       // their last purchases' numbers (each comes twice)
    bool rescue;                               // the game here wanted to put their character back in the level
    // stats for the log
    double errSum, yawErrSum;
    float errMax, yawErrMax;
    int errCount, teleports, slides, healthFixes;
};
Remote g_remote[4];

#pragma pack(push, 1)
struct PktState { PktHeader h; unsigned seq; float pos[3]; float yaw, pitch, health; BYTE alive; unsigned hitAck[4]; int money;
                  unsigned sentMs; };   // sentMs: their clock as it went (how fast they turn: between two of them)
const int kHitInfo = 0x68;                     // the game's damage info (DamageInfo, see the notes)
struct PktHit { PktHeader h; unsigned seq; BYTE victim, source, part; float lost, after; unsigned partOffset; BYTE info[kHitInfo];
                BYTE credit; };   // credit: whose kill it is when no player caused it (a power-up's character: its owner)
#pragma pack(pop)
const int kPktStateSize = (int)sizeof(PktState), kPktHitSize = (int)sizeof(PktHit);
const int kPktHitSizeOld = (int)offsetof(PktHit, credit);
const int kPktStateSizeOld = (int)offsetof(PktState, money), kPktStateSizeMoney = (int)offsetof(PktState, sentMs);

const int kCamYaw = 0xf3b4, kCamPitch = 0xf3b8;    // the player's camera's own angles (radians)
const int kMoney = 0xbc + 0x224;                   // the player's wallet (PlayerMoney)
const uintptr_t kKillMedals = 0x66eb90;            // thiscall (killer, hit, victim): a kill's medals (FIRST BLOOD...)
const int kKillsForMedals = 0xeb24;                //   the killer's kills it counts (its first: FIRST BLOOD)
const int kProxy = 0x104c, kProxyShape = 0x60, kShapePos = 0xe0;   // Havok character proxy -> its shape -> position

unsigned char* PlayerBySlot(int slot) {
    unsigned char* list[8];
    int n = GamePlayers(list, 8);
    for (int i = 0; i < n; ++i)
        if (*reinterpret_cast<int*>(list[i] + 0xdf8) == slot) return list[i];
    return nullptr;
}

float* HealthField(unsigned char* player) {
    auto h = *reinterpret_cast<unsigned char**>(player + 0x105c);
    return h && !IsBadWritePtr(h + 0x50, 4) ? reinterpret_cast<float*>(h + 0x50) : nullptr;
}

// A real teleport: the transform and the Havok shape together (the transform alone slides the character there).
bool TeleportPlayer(unsigned char* player, const float* to) {
    auto xform = *reinterpret_cast<unsigned char**>(player + kXform);
    auto proxy = *reinterpret_cast<unsigned char**>(player + kProxy);
    auto shape = proxy && !IsBadReadPtr(proxy + kProxyShape, 4) ? *reinterpret_cast<unsigned char**>(proxy + kProxyShape) : nullptr;
    if (!xform || !shape || IsBadWritePtr(xform + kXformPos, 12) || IsBadWritePtr(shape + kShapePos, 12)) return false;
    float* tp = reinterpret_cast<float*>(xform + kXformPos);
    float* sp = reinterpret_cast<float*>(shape + kShapePos);
    float lift = sp[1] - tp[1];   // (the shape sits a little above the feet)
    sp[0] = to[0]; sp[1] = to[1] + lift; sp[2] = to[2];
    tp[0] = to[0]; tp[1] = to[1]; tp[2] = to[2];
    return true;
}

float WrapAngle(float a) {
    const float kPi = 3.14159265f;
    while (a > kPi) a -= 2 * kPi;
    while (a < -kPi) a += 2 * kPi;
    return a;
}

// Their inputs, as they come (Receive): the newest one, and every button / key / mouse button that went down meanwhile.
void FreeInputs(int s, const PktInputs* p, int count) {
    Remote& r = g_remote[s];
    for (int i = 0; i < count && i < 16; ++i) {
        unsigned f = p->first + i;
        if (f <= r.lastTick) continue;   // (each packet repeats the last few)
        const Pad& in = p->pads[i];
        r.lastTick = f;
        r.latest = in;
        r.buttonsSince |= in.buttons;
        for (int k = 0; k < 32; ++k) r.keysSince[k] |= in.kbm.keys[k];
        r.mouseSince |= in.kbm.buttons;
    }
    r.heardAt = GetTickCount64();
}

// Milliseconds by the real clock, to a fraction (GetTickCount64 steps by ~16 ms: no good for 33 ms apart).
double PreciseMs() {
    LARGE_INTEGER t;
    Real_QPC(&t);
    return t.QuadPart * 1000.0 / g_freq.QuadPart;
}

void FreeState(int s, const PktState* p, bool withMoney, bool withClock) {
    Remote& r = g_remote[s];
    double now = PreciseMs();
    r.heardAt = GetTickCount64();
    if (r.haveState && p->seq <= r.stateSeq) return;
    if (r.haveState) {   // how fast they're turning: from their last state to this (by their clock, else by when they came)
        double ms = withClock && r.stateSentMs ? (double)(unsigned)(p->sentMs - r.stateSentMs) : now - r.stateAt;
        if (ms >= 10 && ms < 250) {
            float vy = WrapAngle(p->yaw - r.yaw) * 1000.f / (float)ms, vp = (p->pitch - r.pitch) * 1000.f / (float)ms;
            // (speeding up: smoothed, so one late packet doesn't jerk it; slowing down or the other way: at once - a
            // turn that stops shouldn't carry on here)
            r.yawVel = fabsf(vy) < fabsf(r.yawVel) || vy * r.yawVel < 0 ? vy : 0.5f * r.yawVel + 0.5f * vy;
            r.pitchVel = fabsf(vp) < fabsf(r.pitchVel) || vp * r.pitchVel < 0 ? vp : 0.5f * r.pitchVel + 0.5f * vp;
        } else {
            r.yawVel = r.pitchVel = 0;   // (a gap: no guessing)
        }
    }
    r.stateAt = now;
    r.stateSentMs = withClock ? p->sentMs : 0;
    r.haveState = true;
    r.stateSeq = p->seq;
    memcpy(r.pos, p->pos, sizeof(r.pos));
    r.yaw = p->yaw;
    r.pitch = p->pitch;
    r.health = p->health;
    r.alive = p->alive != 0;
    memcpy(r.hitAck, p->hitAck, sizeof(r.hitAck));
    r.haveMoney = withMoney;
    if (withMoney) r.money = p->money;
}

std::vector<PktHit> g_hitsIn;                  // hits from the others, played at the start of my next tick
void FreeHitIn(int s, const PktHit* p, bool withCredit) {
    Remote& r = g_remote[s];
    for (unsigned seen : r.hitSeen) if (seen == p->seq) return;
    r.hitSeen[p->seq % 32] = p->seq;
    if (p->victim >= 4 || p->victim >= g_players) return;
    g_hitsIn.push_back(*p);
    if (!withCredit) g_hitsIn.back().credit = 0xff;
}

// The players' damage (zFamilyGuyPlayer vtable slot 58, 0x6901a0), while the match runs free.
using TakeDamage_t = char(__thiscall*)(void* self, void* info);
TakeDamage_t Real_TakeDamage = nullptr;
volatile LONG g_replayingHit = 0;
CRITICAL_SECTION g_hitLock;
unsigned g_hitSeq = 0;
unsigned g_lastHitOn[4];                       // my last hit on each player (their health from their PC counts once it has it)
int g_hitsBlocked = 0, g_hitsMine = 0;

// Whose kill a hit is that no player caused (the deciding PC - the victim's - has the real source): a summoned character's
// (a power-up's chicken, Joe, Rupert: type 0x11f) is its owner's - the game credits the kill that way (0x968940 in the kill
// credit, 0x5d6450) - and a player's is their own. -1: nobody's (the level's own).
int HitCredit(unsigned char* src) {
    if (!src || IsBadReadPtr(src, 0x14)) return -1;
    unsigned char* owner = src;
    if (*reinterpret_cast<int*>(src + 0x10) == 0x11f)
        owner = IsBadReadPtr(src + 0x8e4, 4) ? nullptr : *reinterpret_cast<unsigned char**>(src + 0x8e4);
    if (!owner || IsBadReadPtr(owner, 0xe00) || *reinterpret_cast<ULONG_PTR*>(owner) != kPlayerVtable) return -1;
    int s = *reinterpret_cast<int*>(owner + 0xdf8);
    return s >= 0 && s < g_players ? s : -1;
}

BYTE g_lastHitInfo[kHitInfo];                  // (test "hurt": the last hit's shape, for a hit of its own)
bool g_haveLastHitInfo = false;
volatile LONG g_testHurtWho = -1;
float g_testHurtAmount = 0;

char __fastcall Free_TakeDamage(void* self, void*, void* info) {
    if (!g_free || g_replayingHit) return Real_TakeDamage(self, info);
    memcpy(g_lastHitInfo, info, kHitInfo);
    g_haveLastHitInfo = true;
    auto victimObj = static_cast<unsigned char*>(self);
    auto src = *static_cast<unsigned char**>(info);
    int victim = *reinterpret_cast<int*>(victimObj + 0xdf8);
    int source = src && !IsBadReadPtr(src, 0xe00) && *reinterpret_cast<ULONG_PTR*>(src) == kPlayerVtable
                     ? *reinterpret_cast<int*>(src + 0xdf8) : -1;
    if (source >= g_players) source = -1;
    int decides = source >= 0 ? source : (victim >= 0 && victim < g_players ? victim : 0);
    static int god = -1;   // (research: FGUYMOD_TEST_GOD - my player takes no hits, to watch a match's enemies)
    if (god < 0) god = SettingInt(L"Debug", L"_", L"FGUYMOD_TEST_GOD", 0);
    if (god && victim == g_slot) return 0;
    if (decides != g_slot) {   // another PC decides this one: it comes from there ('Z')
        ++g_hitsBlocked;
        // (no source: the level's own - out of the world, a kill volume: their character here is somewhere it isn't on
        // their PC - put it where their PC has it at once, FreeApplyRemotes)
        if (source < 0 && victim >= 0 && victim < 4 && victim != g_slot) g_remote[victim].rescue = true;
        return 0;
    }
    float before = PlayerHealth(victimObj);
    char r = Real_TakeDamage(self, info);
    float after = PlayerHealth(victimObj);
    static int calls = 0;
    if (++calls <= 80)
        Log(L"Netplay: free - damage call: player %d by %d, amount %.1f type %d flags %#x, returned %d, health %.1f -> %.1f",
            victim, source, *reinterpret_cast<float*>(static_cast<char*>(info) + 0x1c), *reinterpret_cast<int*>(static_cast<char*>(info) + 0x20),
            *reinterpret_cast<unsigned*>(static_cast<char*>(info) + 0x28), r, before, after);
    if (victim < 0 || victim >= g_players || (!r && after == before)) return r;
    PktHit h{};
    h.h = {'Z', (BYTE)g_slot};
    h.victim = (BYTE)victim;
    h.source = (BYTE)(source < 0 ? 0xff : source);
    int credit = source < 0 ? HitCredit(src) : source;
    h.credit = (BYTE)(credit < 0 ? 0xff : credit);
    h.lost = before - after;
    h.after = after;
    memcpy(h.info, info, kHitInfo);
    auto part = *reinterpret_cast<unsigned char**>(h.info + 0x58);   // the body part hit: inside the victim's object
    if (part >= victimObj && part < victimObj + 0x11000) {
        h.part = 1;
        h.partOffset = (unsigned)(part - victimObj);
    }
    memset(h.info, 0, 4);
    memset(h.info + 0x58, 0, 4);
    EnterCriticalSection(&g_hitLock);
    h.seq = ++g_hitSeq;
    g_lastHitOn[victim] = h.seq;
    LeaveCriticalSection(&g_hitLock);
    SendAll(&h, sizeof(h));
    SendAll(&h, sizeof(h));   // (twice: a lost hit would be gone for good)
    if (++g_hitsMine <= 40)
        Log(L"Netplay: free - my hit #%u: player %d hit by %d (the kill's: %d), health %.1f -> %.1f (type %d)", h.seq, victim,
            source, credit, before, after, *reinterpret_cast<int*>(h.info + 0x20));
    return r;
}

// A player rejoined ('B'): a new game of theirs - its counts start over. Their character plays again here.
void FreeBack(int s) {
    if (s == g_slot || s < 0 || s >= 4) return;
    Remote& r = g_remote[s];
    ULONGLONG now = GetTickCount64();
    if (r.backAt && now - r.backAt < 10000) { r.heardAt = now; return; }   // (the same "I'm back", again)
    r = Remote{};
    r.heardAt = r.backAt = now;
    g_gone[s] = false;
    g_lastHitOn[s] = 0;
    Log(L"Netplay: free - player %d rejoined the match", s);
    if (g_inSession) OverlayShow((PlayerName(s) + L" is back in the match").c_str(), 4000);
}

// A hit played here (from another PC, or the test's) gets its damage info from here, not the stack: the game keeps
// a pointer to it past TakeDamage - the death's event, handled later, reads the killer from it (their kill's medals:
// FIRST BLOOD..., 0x99eec0 -> 0x66eb90). A ring: each stays as it was for the next 31 hits.
BYTE* ReplayInfo() {
    alignas(16) static BYTE ring[32][0x80];
    static unsigned at = 0;
    BYTE* info = ring[at++ % 32];
    memset(info, 0, 0x80);
    return info;
}

// Test pipe "hurt <player> <amount>": that player - mine - takes a hit nobody caused, as the level's own (a fall, a kill
// volume) - decided here and sent like any other. Its shape is the last hit's (one has to have happened).
void FreeTestHurt() {
    LONG who = InterlockedExchange(&g_testHurtWho, -1);
    if (who < 0) return;
    unsigned char* victim = PlayerBySlot(who);
    if (!victim || !g_haveLastHitInfo) {
        Log(L"Netplay: test - hurt player %ld: %s", who, victim ? L"no hit yet to copy" : L"no such player");
        return;
    }
    BYTE* info = ReplayInfo();
    memcpy(info, g_lastHitInfo, kHitInfo);
    memset(info, 0, 4);
    memset(info + 0x58, 0, 4);
    memcpy(info + 4, &g_testHurtAmount, 4);
    memcpy(info + 0x1c, &g_testHurtAmount, 4);
    Log(L"Netplay: test - hurting player %ld by %.0f", who, g_testHurtAmount);
    Free_TakeDamage(victim, nullptr, info);
}

void FreeApplyHits() {
    std::vector<PktHit> hits;
    hits.swap(g_hitsIn);
    for (const PktHit& h : hits) {
        unsigned char* victim = PlayerBySlot(h.victim);
        if (!victim) continue;
        unsigned char* source = h.source < 4 ? PlayerBySlot(h.source) : nullptr;
        // (a power-up's character's: its owner as the source here - the kill is theirs, as on the victim's PC, where the
        // character itself was)
        if (!source && h.credit < 4) source = PlayerBySlot(h.credit);
        BYTE* info = ReplayInfo();
        memcpy(info, h.info, kHitInfo);
        *reinterpret_cast<unsigned char**>(info) = source;
        // (no source here - a power-up's character, Rupert's darts, the chicken: each PC has its own, so it isn't sent -
        // then no body part either: with one, a hit that kills picks its fall from where the source stands, [source+0x38],
        // and crashed on the null. Without one it goes down as from a fall; the victim's PC has the last word on health)
        *reinterpret_cast<unsigned char**>(info + 0x58) = h.part && source ? victim + h.partOffset : nullptr;
        // (decided on the victim's own PC - its health is theirs: their state can come here before the hit and has it in
        // already; played on that it would count twice - a second hit's health off, a death there wasn't. So from the
        // health it had there before the hit, to where it went there)
        float* hf = h.h.slot == h.victim ? HealthField(victim) : nullptr;
        float thereBefore = h.after + h.lost;
        if (hf && *hf > 0 && *hf < thereBefore - 0.5f) *hf = thereBefore;
        float before = PlayerHealth(victim);
        memcpy(g_lastHitInfo, info, kHitInfo);   // (test "hurt": a shape to copy)
        g_haveLastHitInfo = true;
        bool credited = h.source == 0xff && h.credit < 4 && source;   // (a power-up's: its owner as the source)
        int killsBefore = credited ? *reinterpret_cast<int*>(source + kKillsForMedals) : 0;
        InterlockedExchange(&g_replayingHit, 1);
        char r = Real_TakeDamage(victim, info);
        InterlockedExchange(&g_replayingHit, 0);
        if (hf && *hf > 0 && h.after > 0 && fabsf(*hf - h.after) > 0.5f) *hf = h.after;
        // A power-up's kill: the death's event credits a kill's medals (FIRST BLOOD...) to the source only when it's the
        // character itself, as on the victim's PC (0x99eec0 by the damage type) - played here with its owner it didn't,
        // so their medals go here as they did there (0x66eb90: the killer, the hit, the victim; unless it ran after all).
        if (credited && before > 0 && PlayerHealth(victim) <= 0 &&
            *reinterpret_cast<int*>(source + kKillsForMedals) == killsBefore)
            reinterpret_cast<void(__thiscall*)(void*, void*, void*)>(kKillMedals)(source, info, victim);
        Remote& from = g_remote[h.h.slot];
        if (h.seq > from.hitApplied) from.hitApplied = h.seq;
        static int logged = 0;
        if (++logged <= 60)
            Log(L"Netplay: free - hit #%u from player %d's PC: player %d hit by %d%s, health %.1f -> %.1f here (%.1f -> %.1f "
                L"there)%s", h.seq, h.h.slot, h.victim, h.source == 0xff ? -1 : h.source,
                h.source == 0xff && h.credit < 4 ? L" (a power-up's: its owner's kill)" : L"", before, PlayerHealth(victim),
                h.after + h.lost, h.after, r ? L"" : L" - not taken here");
    }
}

// ---- pickups (ammo, health, ...: zFloatingCollectible, vtable 0xa8deb4) ----
// A pickup starts flying to the player who touched it in 0x9beee0 (thiscall: the pickup, the player - it gives them the
// item by an event). While the match runs free only my own player takes pickups here; every one I take goes to the others
// ('U': the pickup's id, which is the same on every PC - the level loads the same way) and they let my character take
// it there, so it's gone (and comes back) everywhere. The pickups' update (vtable method 0x9c0f40) keeps a table of
// them by id for that.
#pragma pack(push, 1)
struct PktPickup { PktHeader h; unsigned seq, id, kind; };
#pragma pack(pop)
const int kPktPickupSize = (int)sizeof(PktPickup);
const uintptr_t kPickupVtable = 0xa8deb4, kCollect = 0x9beee0, kPickupUpdate = 0x9c0f40;
const int kPickupId = 0x18;
using PickupUpdate_t = void(__thiscall*)(void* self, float dt);
PickupUpdate_t Real_PickupUpdate = nullptr;
// The id (+0x18) is the pickup's spot, not the pickup: a spot has one pickup object per kind it can show (+0xa1: the one
// switched on). Which kind it is gets picked once, as the level loads - in lockstep, so the same on every PC (checked,
// also for a game that rejoined) - and a pickup that comes back is the same kind again. A pickup taken somewhere takes
// what shows at that spot here; 'U' says which kind it was there, so a difference would show in the log.
const int kPickupSlots = 512, kPickupKinds = 12;
void* volatile g_pickups[kPickupSlots][kPickupKinds];   // by spot (hashed; checked on use)
volatile LONG g_replayingPickup = 0;
unsigned g_pickupSeq = 0;
std::vector<PktPickup> g_pickupsIn;            // taken on other PCs, played at the start of my next frame
int g_pickupsMine = 0, g_pickupsBlocked = 0;

void __fastcall Free_PickupUpdate(void* self, void*, float dt) {
    unsigned id = *reinterpret_cast<unsigned*>(static_cast<char*>(self) + kPickupId);
    void* volatile* kinds = g_pickups[id % kPickupSlots];
    int free = -1;
    for (int k = 0; k < kPickupKinds; ++k) {
        void* p = kinds[k];
        if (p == self) { free = -2; break; }
        if (free == -1 && (!p || IsBadReadPtr(p, 0xb0) || *static_cast<uintptr_t*>(p) != kPickupVtable)) free = k;
    }
    if (free >= 0) kinds[free] = self;
    Real_PickupUpdate(self, dt);
}

bool PickupShowing(void* p) {   // out to be taken (+0xa3), not flying to anyone yet (+0x74)
    return *(static_cast<char*>(p) + 0xa3) != 0 && *(static_cast<char*>(p) + 0x74) == 0;
}

// What shows at that spot here (nullptr: nothing).
void* PickupById(unsigned id) {
    for (int k = 0; k < kPickupKinds; ++k) {
        void* p = g_pickups[id % kPickupSlots][k];
        if (!p || IsBadReadPtr(p, 0xb0) || *static_cast<uintptr_t*>(p) != kPickupVtable ||
            *reinterpret_cast<unsigned*>(static_cast<char*>(p) + kPickupId) != id)
            continue;
        if (PickupShowing(p)) return p;
    }
    return nullptr;
}

// The original 0x9beee0 (its first 9 bytes, then the rest of it).
__declspec(naked) void __fastcall CollectOriginal(void*, void*, void*) {
    __asm {
        push esi
        mov esi, ecx
        mov eax, dword ptr [esi + 0x84]
        push 0x9beee9
        ret
    }
}

void PickupKind(void* pickup, unsigned* id, unsigned* type);

void __fastcall Free_Collect(void* pickup, void* edx, void* player) {
    if (!g_free || g_replayingPickup || !player || IsBadReadPtr(player, 0xe00) ||
        *static_cast<ULONG_PTR*>(player) != kPlayerVtable) {
        CollectOriginal(pickup, edx, player);
        return;
    }
    int slot = *reinterpret_cast<int*>(static_cast<char*>(player) + 0xdf8);
    if (slot != g_slot) {   // someone else's character here: their own PC says if they take it ('U')
        ++g_pickupsBlocked;
        return;
    }
    unsigned kindId, kindType;
    PickupKind(pickup, &kindId, &kindType);
    CollectOriginal(pickup, edx, player);
    PktPickup u{{'U', (BYTE)g_slot}, ++g_pickupSeq, *reinterpret_cast<unsigned*>(static_cast<char*>(pickup) + kPickupId), kindId};
    SendAll(&u, sizeof(u));
    SendAll(&u, sizeof(u));   // (twice: a lost one would leave it there for the others)
    if (++g_pickupsMine <= 60) Log(L"Netplay: free - I took pickup %#x (kind %#x/%08x)", u.id, kindId, kindType);
}

void FreePickupIn(int s, const PktPickup* p) {
    Remote& r = g_remote[s];
    for (unsigned seen : r.pickupSeen) if (seen == p->seq) return;
    r.pickupSeen[p->seq % 16] = p->seq;
    g_pickupsIn.push_back(*p);
}

// What kind of pickup it is (its asset's id and type word, the same on every PC), for the log.
void PickupKind(void* pickup, unsigned* id, unsigned* type) {
    auto asset = *reinterpret_cast<unsigned**>(static_cast<char*>(pickup) + 0x68);
    *id = *type = 0;
    if (asset && !IsBadReadPtr(asset, 12)) { *id = asset[0]; *type = asset[2]; }
}

// A pickup taken on another PC, played here with their character taking it. A crash inside (a tester's game closed right
// after one, twice - it didn't come back, and didn't happen here) is caught: the pickup is skipped.
bool SafeCollect(void* pickup, void* player) {
    __try {
        CollectOriginal(pickup, nullptr, player);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

void FreeApplyPickups() {
    std::vector<PktPickup> in;
    in.swap(g_pickupsIn);
    for (const PktPickup& u : in) {
        void* pickup = PickupById(u.id);
        unsigned char* player = PlayerBySlot(u.h.slot);
        unsigned kindId = 0, kindType = 0;
        if (pickup) PickupKind(pickup, &kindId, &kindType);
        static int logged = 0;
        bool otherKind = pickup && kindId != u.kind;
        if (++logged <= 60 || otherKind)
            Log(L"Netplay: free - player %d took pickup %#x%s (kind %#x there, %#x/%08x here)%s", u.h.slot, u.id,
                pickup ? L"" : L" (nothing showing there here)", u.kind, kindId, kindType,
                otherKind ? L" - A DIFFERENT KIND HERE" : L"");
        if (!pickup || !player) continue;
        InterlockedExchange(&g_replayingPickup, 1);
        bool ok = SafeCollect(pickup, player);
        InterlockedExchange(&g_replayingPickup, 0);
        if (!ok) Log(L"Netplay: free - taking pickup %#x (kind %#x/%08x) for player %d crashed here - skipped", u.id, kindId,
                     kindType, u.h.slot);
    }
}

// ---- the supply crates' store (weapons, paid with the money kills bring) ----
// The store is a menu: buying calls STOR_SubtractCurrency(player, cost) and STOR_SetMultiplayerWeaponBought(player,
// weapon) (ui.cpp sees them). While the match runs free only my own player buys here: everyone's presses reach their
// character on every PC, so someone else's store opens here too - what it would buy here is dropped
// (NetplayStoreAllowed). What I buy goes to the others ('W'), and their game marks it bought for my character there
// (at its next menu call - before the weapons wheel asks: ui.cpp, NetplayStoreTake). The money itself comes with
// everyone's state ('X'). Research: RESEARCH_NOTES.md "Supply crates".
struct PktBuy { PktHeader h; unsigned seq; int weapon; };
const int kPktBuySize = (int)sizeof(PktBuy);
SRWLOCK g_buyLock = SRWLOCK_INIT;
std::vector<int> g_buysOut;                    // what I bought (the menu's thread) - sent from my next frame
std::vector<std::pair<int, int>> g_buysIn;     // (player, weapon) bought on other PCs - for the menu's thread
unsigned g_buySeq = 0;

// ---- the scoreboard: the host's count ----
// Kills, deaths, score and medal bonuses, as the scoreboard shows them (the game's UpdateMultiPlayerStats: kills player
// +0xe20 and +0xe30, deaths +0xe2c, score +0xe84, bonus +0xe88). Every PC counts them itself from the same hits, but not
// always alike: a medal for a power-up's kill (FIRST BLOOD...) comes only where its character made the kill - the
// victim's PC. So while the match runs free the host's count is everyone's: it goes out ('T') whenever it changes and
// every second, and the others take it. The match clock (elapsed, 0xbadcc0) counts real time on every PC from the same
// tick, so it goes along only to be compared: a PC whose clock is off by more than a quarter of a second says so.
#pragma pack(push, 1)
struct PktStats { PktHeader h; unsigned seq; int v[4][5]; float clock; };
#pragma pack(pop)
const int kPktStatsSize = (int)sizeof(PktStats);
const int kStatField[5] = {0xe20, 0xe30, 0xe2c, 0xe84, 0xe88};
const uintptr_t kMatchClock = 0xbadcc0;   // seconds since the match began (the HUD's timer: the limit minus this)
const wchar_t* const kStatName[5] = {L"kills", L"kills", L"deaths", L"score", L"bonus"};
PktStats g_statsIn{};
bool g_haveStatsIn = false;
unsigned g_statsSeqIn = 0;

void ReadStats(int v[4][5]) {
    for (int s = 0; s < 4; ++s) {
        unsigned char* p = s < g_players ? PlayerBySlot(s) : nullptr;
        for (int k = 0; k < 5; ++k)
            v[s][k] = p && !IsBadReadPtr(p + kStatField[k], 4) ? *reinterpret_cast<int*>(p + kStatField[k]) : 0;
    }
}

void FreeStats() {
    if (g_slot == 0) {   // the host: mine go out
        static ULONGLONG last = 0;
        static int sent[4][5];
        static unsigned seq = 0;
        int v[4][5];
        ReadStats(v);
        ULONGLONG now = GetTickCount64();
        bool changed = memcmp(v, sent, sizeof(v)) != 0;
        if (changed || now - last >= 1000) {
            PktStats t{};
            t.h = {'T', (BYTE)g_slot};
            t.seq = ++seq;
            memcpy(t.v, v, sizeof(v));
            t.clock = *reinterpret_cast<float*>(kMatchClock);
            SendAll(&t, sizeof(t));
            if (changed) SendAll(&t, sizeof(t));   // (twice: a lost one waits a second for the next)
            memcpy(sent, v, sizeof(v));
            last = now;
        }
        return;
    }
    if (!g_haveStatsIn) return;
    g_haveStatsIn = false;
    {   // (the clock: compared, half the ping on the way)
        float host = g_statsIn.clock + InterlockedCompareExchange(&g_pingMs[0], 0, 0) / 2000.f;
        float off = *reinterpret_cast<float*>(kMatchClock) - host;
        static ULONGLONG lastSaid = 0;
        if (fabsf(off) > 0.25f && GetTickCount64() - lastSaid > 30000) {
            lastSaid = GetTickCount64();
            Log(L"Netplay: free - the match clock here is %.2f s %s the host's", fabsf(off), off > 0 ? L"ahead of" : L"behind");
        }
    }
    for (int s = 0; s < g_players && s < 4; ++s) {
        unsigned char* p = PlayerBySlot(s);
        if (!p) continue;
        for (int k = 0; k < 5; ++k) {
            int* f = reinterpret_cast<int*>(p + kStatField[k]);
            if (IsBadWritePtr(f, 4) || *f == g_statsIn.v[s][k]) continue;
            static int logged = 0;
            if (++logged <= 40)
                Log(L"Netplay: free - player %d's %s %d here, %d on the host's scoreboard - the host's", s, kStatName[k], *f,
                    g_statsIn.v[s][k]);
            *f = g_statsIn.v[s][k];
        }
    }
}

void FreeStatsIn(int s, const PktStats* p) {
    if (s != 0 || p->seq <= g_statsSeqIn) return;   // (the host's only; the newest)
    g_statsSeqIn = p->seq;
    g_statsIn = *p;
    g_haveStatsIn = true;
}

void FreeSendBuys() {
    std::vector<int> out;
    AcquireSRWLockExclusive(&g_buyLock);
    out.swap(g_buysOut);
    ReleaseSRWLockExclusive(&g_buyLock);
    for (int weapon : out) {
        PktBuy b{{'W', (BYTE)g_slot}, ++g_buySeq, weapon};
        SendAll(&b, sizeof(b));
        SendAll(&b, sizeof(b));   // (twice: a lost one would leave them without it on the others' PCs)
        Log(L"Netplay: free - I bought weapon %d", weapon);
    }
}

void FreeBuyIn(int s, const PktBuy* p) {
    Remote& r = g_remote[s];
    for (unsigned seen : r.buySeen) if (seen == p->seq) return;
    r.buySeen[p->seq % 8] = p->seq;
    Log(L"Netplay: free - player %d bought weapon %d", s, p->weapon);
    AcquireSRWLockExclusive(&g_buyLock);
    g_buysIn.push_back({s, p->weapon});
    ReleaseSRWLockExclusive(&g_buyLock);
}

bool InstallPickupHooks() {
    const unsigned char start[] = {0x56, 0x8b, 0xf1, 0x8b, 0x86, 0x84, 0x00, 0x00, 0x00};
    auto at = reinterpret_cast<unsigned char*>(kCollect);
    if (IsBadReadPtr(at, sizeof(start)) || memcmp(at, start, sizeof(start)) != 0) {
        Log(L"Netplay: the pickups' code isn't what we expect - pickups not shared");
        return false;
    }
    auto vt = reinterpret_cast<void**>(kPickupVtable);
    int slot = -1;
    for (int i = 0; i < 40 && slot < 0; ++i) if (vt[i] == reinterpret_cast<void*>(kPickupUpdate)) slot = i;
    if (slot < 0) { Log(L"Netplay: no pickup update in its vtable - pickups not shared"); return false; }
    DWORD old;
    VirtualProtect(vt + slot, sizeof(void*), PAGE_READWRITE, &old);
    Real_PickupUpdate = reinterpret_cast<PickupUpdate_t>(vt[slot]);
    vt[slot] = reinterpret_cast<void*>(&Free_PickupUpdate);
    VirtualProtect(vt + slot, sizeof(void*), old, &old);
    VirtualProtect(at, sizeof(start), PAGE_EXECUTE_READWRITE, &old);
    at[0] = 0xE9;
    *reinterpret_cast<int*>(at + 1) = (int)(reinterpret_cast<uintptr_t>(&Free_Collect) - (kCollect + 5));
    memset(at + 5, 0x90, sizeof(start) - 5);
    VirtualProtect(at, sizeof(start), old, &old);
    FlushInstructionCache(GetCurrentProcess(), at, sizeof(start));
    return true;
}

// ---- enemies (NPCs): to be the host's ----
// What's known so far (RESEARCH_NOTES.md "Enemies / NPCs"): every PC's own spawner places its own enemies; an enemy goes
// into the world in 0x60ce30 (thiscall: the enemy, a flag -> placed?) at its spawn point (+0x22c: position, facing), and
// its kind is its template's first word (+0x218 -> [0]). Research knobs: FGUYMOD_TEST_NPCLOG - every PC says which go in;
// FGUYMOD_TEST_NPCBLOCK - the others' PCs (not the host's) place none themselves (to see what their spawners do then).
const uintptr_t kNpcActivate = 0x60ce30;
const unsigned char kNpcActivateStart[] = {0x51, 0x53, 0x56, 0x8b, 0xf1, 0x8b, 0x06};   // push ecx/ebx/esi; mov esi,ecx; mov eax,[esi]
using NpcActivate_t = char(__thiscall*)(void* npc, int flag);
NpcActivate_t NpcActivateOriginal = nullptr;   // (a trampoline: those 7 bytes, then the rest of it)
int g_npcLog = 0, g_npcBlock = 0;

unsigned NpcKind(unsigned char* npc) {
    auto t = *reinterpret_cast<unsigned**>(npc + 0x218);
    return t && !IsBadReadPtr(t, 4) ? t[0] : 0;
}

char __fastcall Free_NpcActivate(unsigned char* npc, void*, int flag) {
    if (!g_free || IsBadReadPtr(npc, 0x260)) return NpcActivateOriginal(npc, flag);
    const float* at = reinterpret_cast<const float*>(npc + 0x22c);
    unsigned id = *reinterpret_cast<unsigned*>(npc + 0x18), kind = NpcKind(npc);
    if (g_npcBlock && g_slot != 0) {
        static int logged = 0;
        if (++logged <= 80)
            Log(L"Netplay: free - enemy %x (kind %x) at (%.1f %.1f %.1f) would go in here (flag %d, from %p) - held", id, kind,
                at[0], at[1], at[2], flag, _ReturnAddress());
        return 0;
    }
    char r = NpcActivateOriginal(npc, flag);
    static int logged = 0;
    if (g_npcLog && ++logged <= 120)
        Log(L"Netplay: free - enemy %x (kind %x) goes in at (%.1f %.1f %.1f, facing %.2f) - flag %d, from %p: %d", id, kind,
            at[0], at[1], at[2], at[3], flag, _ReturnAddress(), r);
    return r;
}

// Spawning (FGUYMOD_TEST_NPCSYNC, being built): every enemy - a wave's, a power-up's chicken - comes from 0x60d080 (cdecl:
// a spawn request -> the enemy, or 0: none free...). Its request (0x6c bytes) is all values the same on every PC - who
// spawns it ([2],[3]: an entity's id), its kind ([4],[5]), where ([0xc..0xe]) and how it faces, flags - but [10], a
// pointer to its team (or none). While the match runs free the host's game spawns them: what it spawns goes out ('E': the
// request, the team, its enemy's id), the others' own spawning is turned down (0, as when none is free - their spawners
// try again later) and they spawn the host's, each linked to the host's (by its id) for what comes next.
const uintptr_t kNpcSpawn = 0x60d080;
const unsigned char kNpcSpawnStart[] = {0x53, 0x56, 0x8b, 0x74, 0x24, 0x0c};   // push ebx; push esi; mov esi, [esp+0xc]
const int kSpawnRequest = 0x6c;
using NpcSpawn_t = unsigned char*(__cdecl*)(void* request);
NpcSpawn_t NpcSpawnOriginal = nullptr;
int g_npcSync = 0;
volatile LONG g_replayingSpawn = 0;
#pragma pack(push, 1)
struct PktSpawn { PktHeader h; unsigned seq; unsigned hostNpc; int team; BYTE hasTeam; BYTE request[kSpawnRequest]; };
#pragma pack(pop)
const int kPktSpawnSize = (int)sizeof(PktSpawn);
std::vector<PktSpawn> g_spawnsIn;              // the host's spawns, played at my next frame
unsigned g_spawnSeen[128], g_spawnSeq = 0;
std::map<unsigned, unsigned char*> g_npcOfHost;   // the host's enemy (its id) -> mine
struct NpcTrack { float pos[3], vel[3], yaw, yawVel; double sentMs; ULONGLONG at, myHitAt; };
std::map<unsigned, NpcTrack> g_npcTrack;          // the host's enemy (its id) -> where it was, how fast, when
int g_spawnsTurnedDown = 0;

unsigned char* __cdecl Free_NpcSpawn(BYTE* request) {
    if (!g_free || !g_npcSync || g_replayingSpawn) return NpcSpawnOriginal(request);
    if (g_slot != 0) {   // the host's game spawns them
        if (++g_spawnsTurnedDown <= 20)
            Log(L"Netplay: free - a spawn here turned down (kind %x:%x) - the host's game spawns them",
                *reinterpret_cast<unsigned*>(request + 0x10), *reinterpret_cast<unsigned*>(request + 0x14));
        return nullptr;
    }
    unsigned char* npc = NpcSpawnOriginal(request);
    if (!npc) return npc;
    PktSpawn e{};
    e.h = {'E', (BYTE)g_slot};
    e.seq = ++g_spawnSeq;
    e.hostNpc = *reinterpret_cast<unsigned*>(npc + 0x18);
    memcpy(e.request, request, kSpawnRequest);
    auto team = *reinterpret_cast<int**>(request + 0x28);
    if (team && !IsBadReadPtr(team, 4)) { e.hasTeam = 1; e.team = *team; }
    memset(e.request + 0x28, 0, 4);
    g_npcOfHost[e.hostNpc] = npc;
    SendAll(&e, sizeof(e));
    SendAll(&e, sizeof(e));   // (twice: a lost one would be an enemy missing over there)
    static int logged = 0;
    if (++logged <= 60)
        Log(L"Netplay: free - enemy %x spawned (kind %x:%x) at (%.1f %.1f %.1f) - sent", e.hostNpc,
            *reinterpret_cast<unsigned*>(request + 0x10), *reinterpret_cast<unsigned*>(request + 0x14),
            *reinterpret_cast<float*>(request + 0x30), *reinterpret_cast<float*>(request + 0x34),
            *reinterpret_cast<float*>(request + 0x38));
    return npc;
}

void FreeSpawnIn(int s, const PktSpawn* p) {
    if (s != 0 || !p->seq) return;   // (the host's; each once - it comes twice, not always in order)
    for (unsigned seen : g_spawnSeen) if (seen == p->seq) return;
    g_spawnSeen[p->seq % 128] = p->seq;
    g_spawnsIn.push_back(*p);
}

void NpcKill(unsigned char* npc);
float* NpcHealthField(unsigned char* npc);
bool NpcAlive(unsigned char* npc);

void FreeApplySpawns() {
    std::vector<PktSpawn> in;
    in.swap(g_spawnsIn);
    for (const PktSpawn& e : in) {
        alignas(16) static BYTE request[kSpawnRequest + 16];
        static int team;
        memcpy(request, e.request, kSpawnRequest);
        team = e.team;
        *reinterpret_cast<int**>(request + 0x28) = e.hasTeam ? &team : nullptr;
        InterlockedExchange(&g_replayingSpawn, 1);
        unsigned char* npc = NpcSpawnOriginal(request);
        InterlockedExchange(&g_replayingSpawn, 0);
        auto was = g_npcOfHost.find(e.hostNpc);   // (a pool id: the host's one by it before is gone there)
        unsigned char* old = was == g_npcOfHost.end() ? nullptr : was->second;
        if (npc) g_npcOfHost[e.hostNpc] = npc;
        else if (old) g_npcOfHost.erase(was);
        g_npcTrack.erase(e.hostNpc);
        for (auto it = g_npcOfHost.begin(); npc && it != g_npcOfHost.end();) {   // (a pool object here too: the host's
            if (it->first != e.hostNpc && it->second == npc) {                  // one it was before is gone here)
                g_npcTrack.erase(it->first);
                it = g_npcOfHost.erase(it);
            } else {
                ++it;
            }
        }
        bool oldInUse = false;
        for (auto& kv : g_npcOfHost) oldInUse |= kv.second == old;
        float* oldHp = old && !oldInUse && NpcAlive(old) ? NpcHealthField(old) : nullptr;
        if (oldHp && *oldHp > 0) {
            Log(L"Netplay: free - the host's enemy %x is a new one: the one it was here (still up) goes down", e.hostNpc);
            NpcKill(old);
        }
        static int logged = 0;
        if (++logged <= 60 || !npc)
            Log(L"Netplay: free - the host's enemy %x (kind %x:%x) at (%.1f %.1f %.1f): %s", e.hostNpc,
                *reinterpret_cast<const unsigned*>(e.request + 0x10), *reinterpret_cast<const unsigned*>(e.request + 0x14),
                *reinterpret_cast<const float*>(e.request + 0x30), *reinterpret_cast<const float*>(e.request + 0x34),
                *reinterpret_cast<const float*>(e.request + 0x38), npc ? L"spawned here" : L"COULDN'T SPAWN HERE");
    }
}

// The enemies' state (being built): the host's game sends where its enemies are and their health ('G', 10 a second);
// the others pull theirs there (a far one straight there), take its health, and one down there goes down here.
// Their damage: an enemy (entity type 0x48) is hurt in the game's damage 0x99eec0 (cdecl: the victim, the hit). A hit
// counts on the PC of the player who made it - at once there, and the host's game gets it ('V'); one another player
// made, played here from their input, doesn't count here (it comes from their PC); the rest (the enemies' own, the
// level's) the host's game decides.
const uintptr_t kEntityDamage = 0x99eec0;
const unsigned char kEntityDamageStart[] = {0x83, 0xec, 0x34, 0x53, 0x8b, 0x5c, 0x24, 0x3c};   // sub esp,34; push ebx; mov ebx,[esp+3c]
using EntityDamage_t = void(__cdecl*)(void* victim, void* info);
EntityDamage_t EntityDamageOriginal = nullptr;
volatile LONG g_replayingNpcHit = 0;
const int kNpcHealthObj = 0x420, kNpcHealth = 0x60;
#pragma pack(push, 1)
struct NpcState { unsigned hostNpc; float pos[3]; float health; float yaw; };
struct PktNpcStates { PktHeader h; unsigned seq; unsigned sentMs; BYTE count; NpcState s[48]; };
struct PktNpcHit { PktHeader h; unsigned seq; unsigned hostNpc; float lost; BYTE info[kHitInfo]; };
struct PktNpcDown { PktHeader h; unsigned seq; unsigned hostNpc; };
#pragma pack(pop)
const int kPktNpcStatesHead = (int)offsetof(PktNpcStates, s), kPktNpcHitSize = (int)sizeof(PktNpcHit);
const int kPktNpcDownSize = (int)sizeof(PktNpcDown);
unsigned g_npcHitSeq = 0, g_npcHitSeen[4][32], g_npcDownSeq = 0, g_npcDownSeen[64];
BYTE g_lastNpcHitInfo[kHitInfo];   // (the last hit on an enemy seen here, any: a shape for a kill of its own)
bool g_haveLastNpcHitInfo = false;
unsigned g_npcStatesSeqIn = 0;

bool NpcAlive(unsigned char* npc) {   // (still an enemy object - a pool one: it may be in use for another by now)
    return npc && !IsBadReadPtr(npc, 0x430) && *reinterpret_cast<int*>(npc + 0x20) == 0x48;
}
float* NpcHealthField(unsigned char* npc) {
    auto o = *reinterpret_cast<unsigned char**>(npc + kNpcHealthObj);
    return o && !IsBadWritePtr(o + kNpcHealth, 8) ? reinterpret_cast<float*>(o + kNpcHealth) : nullptr;
}
float* NpcPosField(unsigned char* npc) {
    auto x = *reinterpret_cast<unsigned char**>(npc + 0x38);
    return x && !IsBadWritePtr(x + 0x30, 12) ? reinterpret_cast<float*>(x + 0x30) : nullptr;
}
// Moving an enemy: where it is is its physics body's (a Havok hkpRigidBody at [[npc+0x124] (its
// zNPCCombatCollisionRigidBodyListener) +8]: its transform +0x120, its swept transform's centres +0x130/+0x140, two more
// +0x1d0/+0x1e0) - the game copies it to the transform each frame; so all of them by the same, with the transforms
// (+0x38, +0x5c) and the steering's (zNPCSingleSteering at +0x418, +0xc4).
const ULONG_PTR kNpcBodyListenerVtable = 0x9f2308, kNpcBodyVtable = 0xa5ae74, kNpcSteeringVtable = 0x9f5644;
void NpcShift(unsigned char* p, float dx, float dy, float dz) {
    if (!p || IsBadWritePtr(p, 12)) return;
    auto f = reinterpret_cast<float*>(p);
    f[0] += dx;
    f[1] += dy;
    f[2] += dz;
}
unsigned char* NpcObj(unsigned char* p, int at, ULONG_PTR vtable, int size) {
    if (!p || IsBadReadPtr(p + at, 4)) return nullptr;
    auto o = *reinterpret_cast<unsigned char**>(p + at);
    if (!o || IsBadReadPtr(o, size)) return nullptr;
    return !vtable || *reinterpret_cast<ULONG_PTR*>(o) == vtable ? o : nullptr;
}
void NpcMoveBy(unsigned char* npc, float dx, float dy, float dz) {
    if (unsigned char* body = NpcObj(NpcObj(npc, 0x124, kNpcBodyListenerVtable, 0xc), 8, kNpcBodyVtable, 0x1f0))
        for (int o : {0x120, 0x130, 0x140, 0x1d0, 0x1e0}) NpcShift(body + o, dx, dy, dz);
    if (unsigned char* steer = NpcObj(npc, 0x418, kNpcSteeringVtable, 0xd0)) NpcShift(steer + 0xc4, dx, dy, dz);
    for (int at : {0x38, 0x5c})
        if (unsigned char* m = NpcObj(npc, at, 0, 0x3c)) NpcShift(m + 0x30, dx, dy, dz);
}
// Which way it faces: its transform's forward row (+0x20), turned each frame toward its steering's heading (+0xbc /
// +0xc0, 0..2 pi; forward +0xa0); so turning one = turning the rows of both transforms (+0x38, +0x5c) about up, and
// the steering's with them. Yaw = atan2(forward x, forward z).
float NpcYaw(unsigned char* npc) {
    unsigned char* m = NpcObj(npc, 0x38, 0, 0x3c);
    if (!m) return 0;
    auto f = reinterpret_cast<float*>(m + 0x20);
    return atan2f(f[0], f[2]);
}
void NpcTurnBy(unsigned char* npc, float a) {
    float c = cosf(a), s = sinf(a);
    for (int at : {0x38, 0x5c}) {
        unsigned char* m = NpcObj(npc, at, 0, 0x3c);
        if (!m || IsBadWritePtr(m, 0x30)) continue;
        for (int row = 0; row < 3; ++row) {
            auto v = reinterpret_cast<float*>(m + row * 0x10);
            float x = v[0], z = v[2];
            v[0] = x * c + z * s;
            v[2] = -x * s + z * c;
        }
    }
    if (unsigned char* st = NpcObj(npc, 0x418, kNpcSteeringVtable, 0xd0)) {   // (its steering's heading too - it turns
        if (IsBadWritePtr(st + 0xa0, 0x24)) return;                           // it back to that one otherwise)
        auto v = reinterpret_cast<float*>(st + 0xa0);
        float x = v[0], z = v[2];
        v[0] = x * c + z * s;
        v[2] = -x * s + z * c;
        for (int at : {0xbc, 0xc0}) {   // (0..2 pi)
            auto h = reinterpret_cast<float*>(st + at);
            float t = *h + a;
            while (t >= 6.2831853f) t -= 6.2831853f;
            while (t < 0) t += 6.2831853f;
            *h = t;
        }
    }
}
unsigned HostNpcOf(unsigned char* npc) {   // (mine -> the host's id)
    for (auto& kv : g_npcOfHost) if (kv.second == npc) return kv.first;
    return 0;
}
// Down here too (the host's game has it down): a hit for more than it has left, as the level's (no one's kill).
void NpcKill(unsigned char* npc) {
    float* hp = NpcHealthField(npc);
    if (!hp || *hp <= 0) return;
    const BYTE* shape = g_haveLastNpcHitInfo ? g_lastNpcHitInfo : g_haveLastHitInfo ? g_lastHitInfo : nullptr;
    if (!shape) {   // (no hit seen here yet to copy: its health at least)
        *hp = 0;
        Log(L"Netplay: free - enemy %x down: no hit seen here yet to play - its health set to 0",
            *reinterpret_cast<unsigned*>(npc + 0x18));
        return;
    }
    BYTE* info = ReplayInfo();
    memcpy(info, shape, kHitInfo);
    memset(info, 0, 4);
    memset(info + 0x58, 0, 4);
    float amount = *hp + 1000;
    memcpy(info + 4, &amount, 4);
    memcpy(info + 0x1c, &amount, 4);
    InterlockedExchange(&g_replayingNpcHit, 1);
    EntityDamageOriginal(npc, info);
    InterlockedExchange(&g_replayingNpcHit, 0);
}
void NpcSendDown(unsigned char* npc, float before) {   // (the host: one of its enemies just went down)
    float* hp = NpcHealthField(npc);
    if (g_slot != 0 || !hp || before <= 0 || *hp > 0) return;
    PktNpcDown k{};
    k.h = {'K', (BYTE)g_slot};
    k.seq = ++g_npcDownSeq;
    k.hostNpc = *reinterpret_cast<unsigned*>(npc + 0x18);
    SendAll(&k, sizeof(k));
    SendAll(&k, sizeof(k));
}
void FreeNpcDownIn(int s, const PktNpcDown* p) {
    if (s != 0 || g_slot == 0 || !p->seq) return;
    for (unsigned seen : g_npcDownSeen) if (seen == p->seq) return;
    g_npcDownSeen[p->seq % 64] = p->seq;
    auto it = g_npcOfHost.find(p->hostNpc);
    unsigned char* npc = it == g_npcOfHost.end() ? nullptr : it->second;
    if (NpcAlive(npc)) NpcKill(npc);
    g_npcTrack.erase(p->hostNpc);
}

void __cdecl Free_EntityDamage(unsigned char* victim, unsigned char* info) {
    if (!g_free || !g_npcSync || g_replayingNpcHit || !victim || IsBadReadPtr(victim, 0x24) ||
        *reinterpret_cast<int*>(victim + 0x20) != 0x48 || !info || IsBadReadPtr(info, kHitInfo)) {
        EntityDamageOriginal(victim, info);
        return;
    }
    auto src = *reinterpret_cast<unsigned char**>(info);
    int source = src && !IsBadReadPtr(src, 0xe00) && *reinterpret_cast<ULONG_PTR*>(src) == kPlayerVtable
                     ? *reinterpret_cast<int*>(src + 0xdf8) : -1;
    memcpy(g_lastNpcHitInfo, info, kHitInfo);
    g_haveLastNpcHitInfo = true;
    float* hp = NpcHealthField(victim);
    float before = hp ? *hp : -1;
    if (source == g_slot) {   // mine: here at once, and the host's game from me
        BYTE hit[kHitInfo];
        memcpy(hit, info, kHitInfo);   // (as it came: the game's damage changes it)
        EntityDamageOriginal(victim, info);
        NpcSendDown(victim, before);
        unsigned host = g_slot == 0 ? 0 : HostNpcOf(victim);
        static int logged = 0;
        if (g_slot != 0 && ++logged <= 200)
            Log(L"Netplay: free - my hit on enemy %x (the host's %x): health %.1f -> %.1f", *reinterpret_cast<unsigned*>(victim + 0x18),
                host, before, hp ? *hp : -1);
        if (host) {
            g_npcTrack[host].myHitAt = GetTickCount64();
            PktNpcHit v{};
            v.h = {'V', (BYTE)g_slot};
            v.seq = ++g_npcHitSeq;
            v.hostNpc = host;
            v.lost = hp && before > *hp ? before - *hp : 0.f;
            memcpy(v.info, hit, kHitInfo);
            memset(v.info, 0, 4);
            memset(v.info + 0x58, 0, 4);
            SendAll(&v, sizeof(v));
            SendAll(&v, sizeof(v));
        }
        return;
    }
    if (g_slot == 0 && source < 0) {   // the host's game: the rest
        EntityDamageOriginal(victim, info);
        NpcSendDown(victim, before);
        return;
    }
    // (another player's, played here from their input: theirs counts on their PC; or not the host: the host's state)
}

void FreeNpcHitIn(int s, const PktNpcHit* p) {
    if (g_slot != 0 || s <= 0 || s >= 4) return;
    for (unsigned seen : g_npcHitSeen[s]) if (seen == p->seq) return;
    g_npcHitSeen[s][p->seq % 32] = p->seq;
    auto it = g_npcOfHost.find(p->hostNpc);
    unsigned char* npc = it == g_npcOfHost.end() ? nullptr : it->second;
    unsigned char* player = PlayerBySlot(s);
    if (!NpcAlive(npc) || !player) {
        Log(L"Netplay: free - player %d's hit on enemy %x: no such enemy here", s, p->hostNpc);
        return;
    }
    BYTE* info = ReplayInfo();
    memcpy(info, p->info, kHitInfo);
    *reinterpret_cast<unsigned char**>(info) = player;   // (theirs: their kill, their points)
    // (the health it took there, here too - the game's damage works it out a little differently on each PC: played
    // with the health well up, then set; one that downs it played as a hit for more than it has left)
    float* hp = NpcHealthField(npc);
    if (!hp) return;
    float before = *hp, after = before - p->lost;
    if (before <= 0) {
        Log(L"Netplay: free - player %d's hit on enemy %x: already down here", s, p->hostNpc);
        return;
    }
    if (after <= 0) {
        float amount = before + 1000;
        memcpy(info + 4, &amount, 4);
        memcpy(info + 0x1c, &amount, 4);
    } else {
        *hp = before + 100000;
    }
    InterlockedExchange(&g_replayingNpcHit, 1);
    EntityDamageOriginal(npc, info);
    InterlockedExchange(&g_replayingNpcHit, 0);
    if (after > 0) *hp = after;
    NpcSendDown(npc, before);
    static int logged = 0;
    if (++logged <= 200)
        Log(L"Netplay: free - player %d's hit on enemy %x: health %.1f -> %.1f", s, p->hostNpc, before, hp ? *hp : -1);
}

void FreeSendNpcStates() {
    static ULONGLONG last = 0;
    ULONGLONG now = GetTickCount64();
    if (g_slot != 0 || now - last < 100) return;
    last = now;
    PktNpcStates g{};
    g.h = {'G', (BYTE)g_slot};
    static unsigned seq = 0;
    g.seq = ++seq;
    g.sentMs = (unsigned)(ULONGLONG)PreciseMs();
    for (auto& kv : g_npcOfHost) {
        if (g.count >= 48) break;
        unsigned char* npc = kv.second;
        if (!NpcAlive(npc)) continue;
        float* pos = NpcPosField(npc);
        float* hp = NpcHealthField(npc);
        if (!pos || !hp) continue;
        NpcState& e = g.s[g.count++];
        e.hostNpc = kv.first;
        memcpy(e.pos, pos, 12);
        e.health = *hp;
        e.yaw = NpcYaw(npc);
    }
    if (g.count) SendAll(&g, kPktNpcStatesHead + g.count * (int)sizeof(NpcState));
}

float g_npcOffSum = 0, g_npcOffMax = 0, g_npcOffY = 0, g_npcOffYaw = 0;   // (how far off the enemies were when the host's came: the log's, 5 s)
unsigned g_npcOffCount = 0;
void FreeNpcStatesIn(int s, const PktNpcStates* p, int n) {
    if (s != 0 || g_slot == 0 || p->seq <= g_npcStatesSeqIn) return;
    static ULONGLONG lastLog = 0;
    ULONGLONG now = GetTickCount64();
    if (now - lastLog >= 5000) {
        if (g_npcOffCount)
            Log(L"Netplay: free - enemies: %.2f off on average along the ground, %.2f at most, height %.2f off on "
                L"average, facing %.1f deg off on average (%u updates)", g_npcOffSum / g_npcOffCount, g_npcOffMax,
                g_npcOffY / g_npcOffCount, g_npcOffYaw / g_npcOffCount * 57.2958f, g_npcOffCount);
        lastLog = now;
        g_npcOffSum = g_npcOffMax = g_npcOffY = g_npcOffYaw = 0;
        g_npcOffCount = 0;
    }
    g_npcStatesSeqIn = p->seq;
    int count = p->count;
    if (kPktNpcStatesHead + count * (int)sizeof(NpcState) > n) count = (n - kPktNpcStatesHead) / (int)sizeof(NpcState);
    for (int i = 0; i < count && i < 48; ++i) {
        const NpcState& e = p->s[i];
        auto it = g_npcOfHost.find(e.hostNpc);
        unsigned char* npc = it == g_npcOfHost.end() ? nullptr : it->second;
        if (!NpcAlive(npc)) continue;
        float* pos = NpcPosField(npc);
        float* hp = NpcHealthField(npc);
        if (!pos || !hp) continue;
        NpcTrack& t = g_npcTrack[e.hostNpc];
        float gap = (float)(p->sentMs - t.sentMs) / 1000.f;   // (how fast: from the host's last one, its clock)
        float jump = sqrtf((e.pos[0] - t.pos[0]) * (e.pos[0] - t.pos[0]) + (e.pos[1] - t.pos[1]) * (e.pos[1] - t.pos[1]) +
                           (e.pos[2] - t.pos[2]) * (e.pos[2] - t.pos[2]));
        bool steady = t.at && gap > 0.02f && gap < 1.f;
        for (int k = 0; k < 3; ++k) t.vel[k] = steady && jump < 8 ? (e.pos[k] - t.pos[k]) / gap : 0.f;
        float turn = WrapAngle(e.yaw - t.yaw);
        t.yawVel = steady && fabsf(turn) < 1.5f ? turn / gap : 0.f;
        t.yaw = e.yaw;
        memcpy(t.pos, e.pos, 12);
        t.sentMs = p->sentMs;
        t.at = now;
        LONG ping = InterlockedCompareExchange(&g_pingMs[0], 0, 0);
        float ahead = ping > 0 ? ping / 2000.f : 0.f;   // (the host's now: how far off mine is)
        float dx = e.pos[0] + t.vel[0] * ahead - pos[0], dy = e.pos[1] + t.vel[1] * ahead - pos[1],
              dz = e.pos[2] + t.vel[2] * ahead - pos[2];
        float d = sqrtf(dx * dx + dz * dz);
        g_npcOffSum += d;
        g_npcOffY += fabsf(dy);
        g_npcOffYaw += fabsf(WrapAngle(e.yaw + t.yawVel * ahead - NpcYaw(npc)));
        g_npcOffMax = d > g_npcOffMax ? d : g_npcOffMax;
        ++g_npcOffCount;
        if (e.health <= 0 && *hp > 0) {   // down there: down here
            NpcKill(npc);
        } else if (e.health > 0 && *hp > 0 && (e.health <= *hp || now - t.myHitAt > 1000)) {
            *hp = e.health;   // (not up again: one down here by my hit, or my hit not there yet - the host's one before it)
        }
    }
}

// Each frame (not the host): each enemy pulled toward where the host's is by now (its last place, on at its speed for
// that long and half the ping, a quarter second at most) - a part of the way each frame along the ground, a far one
// straight there; and turned the way it faces there (on at its turning speed, for a shorter guess).
void FreeNpcFollow(float dt) {
    if (g_slot == 0 || g_npcTrack.empty()) return;
    ULONGLONG now = GetTickCount64();
    LONG ping = InterlockedCompareExchange(&g_pingMs[0], 0, 0);
    float k = 1.f - expf(-dt * 8.f);
    for (auto& kv : g_npcTrack) {
        const NpcTrack& t = kv.second;
        if (!t.at || now - t.at > 1000) continue;   // (the host's stopped sending it: gone there)
        auto it = g_npcOfHost.find(kv.first);
        unsigned char* npc = it == g_npcOfHost.end() ? nullptr : it->second;
        if (!NpcAlive(npc)) continue;
        float* pos = NpcPosField(npc);
        if (!pos) continue;
        float ahead = (float)(now - t.at) / 1000.f + (ping > 0 ? ping / 2000.f : 0.f);
        if (ahead > 0.25f) ahead = 0.25f;
        float dx = t.pos[0] + t.vel[0] * ahead - pos[0], dy = t.pos[1] + t.vel[1] * ahead - pos[1],
              dz = t.pos[2] + t.vel[2] * ahead - pos[2];
        float d = sqrtf(dx * dx + dy * dy + dz * dz);
        if (d > 8) NpcMoveBy(npc, dx, dy, dz);
        else if (dx * dx + dz * dz > 0.0004f) NpcMoveBy(npc, dx * k, 0, dz * k);   // (along the ground: its height its
                                                                                  // physics' - pulled, it sinks in)
        float ahYaw = ahead > 0.15f ? 0.15f : ahead;   // (turning on: a shorter guess)
        float turn = WrapAngle(t.yaw + t.yawVel * ahYaw - NpcYaw(npc));
        if (fabsf(turn) > 0.002f) NpcTurnBy(npc, turn);   // (all the way: its AI here aims it its own way each frame)
    }
}

// Test pipe "killnpcs" (the host): every enemy here down, as the level's - and so on every PC (to get through a wave).
volatile LONG g_testKillNpcs = 0;
void FreeTestKillNpcs() {
    if (!InterlockedExchange(&g_testKillNpcs, 0)) return;
    if (g_slot != 0) { Log(L"Netplay: test - killnpcs: the host's game only"); return; }
    int n = 0;
    for (auto& kv : g_npcOfHost) {
        unsigned char* npc = kv.second;
        float* hp = NpcAlive(npc) ? NpcHealthField(npc) : nullptr;
        if (!hp || *hp <= 0) continue;
        float before = *hp;
        NpcKill(npc);
        NpcSendDown(npc, before);
        ++n;
    }
    Log(L"Netplay: test - killnpcs: %d enemies down", n);
}

bool InstallEntityDamageHook() {
    auto at = reinterpret_cast<unsigned char*>(kEntityDamage);
    if (IsBadReadPtr(at, sizeof(kEntityDamageStart)) || memcmp(at, kEntityDamageStart, sizeof(kEntityDamageStart)) != 0) {
        Log(L"Netplay: the game's damage isn't what we expect - enemies' damage not shared");
        return false;
    }
    auto tramp = static_cast<unsigned char*>(VirtualAlloc(nullptr, 32, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
    if (!tramp) return false;
    memcpy(tramp, kEntityDamageStart, sizeof(kEntityDamageStart));
    tramp[8] = 0xE9;
    *reinterpret_cast<int*>(tramp + 9) = (int)((kEntityDamage + 8) - (reinterpret_cast<uintptr_t>(tramp) + 13));
    EntityDamageOriginal = reinterpret_cast<EntityDamage_t>(tramp);
    DWORD old;
    VirtualProtect(at, sizeof(kEntityDamageStart), PAGE_EXECUTE_READWRITE, &old);
    at[0] = 0xE9;
    *reinterpret_cast<int*>(at + 1) = (int)(reinterpret_cast<uintptr_t>(&Free_EntityDamage) - (kEntityDamage + 5));
    at[5] = at[6] = at[7] = 0x90;
    VirtualProtect(at, sizeof(kEntityDamageStart), old, &old);
    FlushInstructionCache(GetCurrentProcess(), at, sizeof(kEntityDamageStart));
    return true;
}

bool InstallNpcSpawnHook() {
    auto at = reinterpret_cast<unsigned char*>(kNpcSpawn);
    if (IsBadReadPtr(at, sizeof(kNpcSpawnStart)) || memcmp(at, kNpcSpawnStart, sizeof(kNpcSpawnStart)) != 0) {
        Log(L"Netplay: the enemies' spawning isn't what we expect - not the host's");
        return false;
    }
    auto tramp = static_cast<unsigned char*>(VirtualAlloc(nullptr, 32, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
    if (!tramp) return false;
    memcpy(tramp, kNpcSpawnStart, sizeof(kNpcSpawnStart));
    tramp[6] = 0xE9;
    *reinterpret_cast<int*>(tramp + 7) = (int)((kNpcSpawn + 6) - (reinterpret_cast<uintptr_t>(tramp) + 11));
    NpcSpawnOriginal = reinterpret_cast<NpcSpawn_t>(tramp);
    DWORD old;
    VirtualProtect(at, sizeof(kNpcSpawnStart), PAGE_EXECUTE_READWRITE, &old);
    at[0] = 0xE9;
    *reinterpret_cast<int*>(at + 1) = (int)(reinterpret_cast<uintptr_t>(&Free_NpcSpawn) - (kNpcSpawn + 5));
    at[5] = 0x90;
    VirtualProtect(at, sizeof(kNpcSpawnStart), old, &old);
    FlushInstructionCache(GetCurrentProcess(), at, sizeof(kNpcSpawnStart));
    return true;
}

bool InstallNpcHooks() {
    auto at = reinterpret_cast<unsigned char*>(kNpcActivate);
    if (IsBadReadPtr(at, sizeof(kNpcActivateStart)) || memcmp(at, kNpcActivateStart, sizeof(kNpcActivateStart)) != 0) {
        Log(L"Netplay: the enemies' code isn't what we expect - enemies not watched");
        return false;
    }
    auto tramp = static_cast<unsigned char*>(VirtualAlloc(nullptr, 32, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
    if (!tramp) return false;
    memcpy(tramp, kNpcActivateStart, sizeof(kNpcActivateStart));
    tramp[7] = 0xE9;
    *reinterpret_cast<int*>(tramp + 8) = (int)((kNpcActivate + 7) - (reinterpret_cast<uintptr_t>(tramp) + 12));
    NpcActivateOriginal = reinterpret_cast<NpcActivate_t>(tramp);
    DWORD old;
    VirtualProtect(at, sizeof(kNpcActivateStart), PAGE_EXECUTE_READWRITE, &old);
    at[0] = 0xE9;
    *reinterpret_cast<int*>(at + 1) = (int)(reinterpret_cast<uintptr_t>(&Free_NpcActivate) - (kNpcActivate + 5));
    at[5] = at[6] = 0x90;
    VirtualProtect(at, sizeof(kNpcActivateStart), old, &old);
    FlushInstructionCache(GetCurrentProcess(), at, sizeof(kNpcActivateStart));
    return true;
}

// Everyone else's characters: towards where their own PC has them.
void FreeApplyRemotes(float dt) {
    for (int s = 0; s < g_players && s < 4; ++s) {
        Remote& r = g_remote[s];
        if (s == g_slot || g_gone[s] || !r.haveState) continue;
        unsigned char* p = PlayerBySlot(s);
        if (!p) continue;
        // their money: as their PC has it (kills pay there; what they spend at a supply crate comes off there)
        int* money = reinterpret_cast<int*>(p + kMoney);
        if (r.haveMoney && !IsBadWritePtr(money, 4)) *money = r.money;
        float health = PlayerHealth(p);
        if (!r.alive || health <= 0) continue;   // (down, or respawning: their own input brings them back)
        float* yaw = reinterpret_cast<float*>(p + kCamYaw);
        float* pitch = reinterpret_cast<float*>(p + kCamPitch);
        float k = dt * 20 < 1 ? dt * 20 : 1;
        // Where their aim is by now: their last state on at the speed they were turning - its age, half its way here (a
        // quarter of the ping: all of it overshot a quick stop by 20-30 degrees at 220 ms) and the easing's own 1/20 s -
        // at most 0.15 s ahead. Followed smoothly, so a turn shows as a turn, not steps 30 times a second.
        LONG ping = InterlockedCompareExchange(&g_pingMs[s], 0, 0);
        float ahead = (float)(PreciseMs() - r.stateAt) / 1000.f + (ping > 0 ? ping / 4000.f : 0) + 0.05f;
        if (ahead > 0.15f) ahead = 0.15f;
        static int predict = -1;   // (test: FGUYMOD_TEST_NOAIMPREDICT - followed as before, to compare)
        if (predict < 0) predict = SettingInt(L"Debug", L"_", L"FGUYMOD_TEST_NOAIMPREDICT", 0) ? 0 : 1;
        if (!predict) ahead = 0;
        float wantYaw = WrapAngle(r.yaw + r.yawVel * ahead), wantPitch = r.pitch + r.pitchVel * ahead;
        float yawErr = fabsf(WrapAngle(wantYaw - *yaw));   // (before this frame's step: how far their aim was off)
        r.yawErrSum += yawErr;
        if (yawErr > r.yawErrMax) r.yawErrMax = yawErr;
        *yaw = WrapAngle(*yaw + WrapAngle(wantYaw - *yaw) * k);
        *pitch += (wantPitch - *pitch) * k;
        float* pos = const_cast<float*>(PlayerPos(p));
        if (!pos) continue;
        float dx = r.pos[0] - pos[0], dy = r.pos[1] - pos[1], dz = r.pos[2] - pos[2];
        float dist = sqrtf(dx * dx + dy * dy + dz * dz);
        r.errSum += dist;
        if (dist > r.errMax) r.errMax = dist;
        ++r.errCount;
        // Far off (a respawn, a big lag), or above / below where their PC has them (fell somewhere they didn't - a slide
        // can't come back up through the floor - or the game wanted to put them back in the level: that's their PC's call,
        // FreeRescue): straight there - unless someone's standing there.
        bool jump = dist > 4 || fabsf(dy) > 1.5f || r.rescue;
        r.rescue = false;
        if (jump) {
            bool clear = true;
            unsigned char* list[8];
            int n = GamePlayers(list, 8);
            for (int i = 0; i < n; ++i) {
                const float* o = list[i] == p ? nullptr : PlayerPos(list[i]);
                if (o && fabsf(o[0] - r.pos[0]) < 1.6f && fabsf(o[2] - r.pos[2]) < 1.6f && fabsf(o[1] - r.pos[1]) < 2.5f) clear = false;
            }
            if (clear && TeleportPlayer(p, r.pos)) ++r.teleports;
        } else if (dist > 0.05f) {   // close: slide the rest of the way (the game's own collision on the way)
            float m = dt * 8 < 1 ? dt * 8 : 1;
            pos[0] += dx * m;
            pos[1] += dy * m;
            pos[2] += dz * m;
            ++r.slides;
        }
        // their health: as their PC has it - once it has counted my last hit on them (before that it's behind mine)
        if (r.health > 0 && fabsf(r.health - health) > 0.5f && r.hitAck[g_slot] >= g_lastHitOn[s])
            if (float* hf = HealthField(p)) { *hf = r.health; ++r.healthFixes; }
    }
}

void FreeSendState() {
    static ULONGLONG last = 0;
    static unsigned seq = 0;
    ULONGLONG now = GetTickCount64();
    if (now - last < 33) return;
    last = now;
    unsigned char* me = PlayerBySlot(g_slot);
    const float* pos = me ? PlayerPos(me) : nullptr;
    if (!pos) return;
    PktState st{};
    st.h = {'X', (BYTE)g_slot};
    st.seq = ++seq;
    memcpy(st.pos, pos, sizeof(st.pos));
    st.yaw = *reinterpret_cast<float*>(me + kCamYaw);
    st.pitch = *reinterpret_cast<float*>(me + kCamPitch);
    st.health = PlayerHealth(me);
    st.alive = st.health > 0 ? 1 : 0;
    for (int s = 0; s < 4; ++s) st.hitAck[s] = g_remote[s].hitApplied;   // (the hits my health has in it)
    st.money = PlayerMoney(me);
    st.sentMs = (unsigned)(ULONGLONG)PreciseMs();
    SendAll(&st, sizeof(st));
}

// Their input to play this frame: the newest, plus whatever went down since my last frame. Their camera doesn't turn
// from it here (it follows their PC's angles: FreeApplyRemotes).
Pad FreeRemotePad(int s) {
    Remote& r = g_remote[s];
    if (g_gone[s]) return Pad{};
    Pad p = r.latest;
    p.buttons |= r.buttonsSince;
    r.buttonsSince = 0;
    for (int k = 0; k < 32; ++k) { p.kbm.keys[k] |= r.keysSince[k]; r.keysSince[k] = 0; }
    p.kbm.buttons |= r.mouseSince;
    r.mouseSince = 0;
    p.rx = p.ry = 0;
    p.kbm.dx = p.kbm.dy = 0;
    p.kbm.sync = 0;
    // START: only for their death menu's weapons screen (it respawns them - their PC sends it only then). Their character
    // here may not be at the same point (it isn't down here yet, or is up already): then it would pause my game.
    unsigned char* them = PlayerBySlot(s);
    if (!them || PlayerHealth(them) > 0) p.buttons &= ~XINPUT_GAMEPAD_START;
    return p;
}

// Everyone lets go of the lockstep at this tick (the same one on every PC).
void FreeStart() {
    LARGE_INTEGER real;
    Real_QPC(&real);   // the game thread's clock carries on from the fixed one, in real time from here
    g_qpcOffset = kClockBaseSeconds * g_freq.QuadPart + TickTime(g_freq.QuadPart) + (++g_callsThisTick) - real.QuadPart;
    g_tickCountOffset = (LONGLONG)FixedMs() - (LONGLONG)Real_GetTickCount();
    g_timeGetTimeOffset = (LONGLONG)FixedMs() - (LONGLONG)Real_timeGetTime();
    ULONGLONG now = GetTickCount64();
    for (int s = 0; s < 4; ++s) {
        Remote& r = g_remote[s];
        r = Remote{};
        r.lastTick = g_tick;   // (their lockstep inputs ahead of here: not played)
        r.latest = g_current[s];
        r.heardAt = now;
    }
    g_statsSeqIn = 0;
    g_haveStatsIn = false;
    memset(g_spawnSeen, 0, sizeof(g_spawnSeen));
    memset(g_npcHitSeen, 0, sizeof(g_npcHitSeen));
    memset(g_npcDownSeen, 0, sizeof(g_npcDownSeen));
    g_haveLastNpcHitInfo = false;
    g_npcStatesSeqIn = 0;
    g_spawnsIn.clear();
    g_npcOfHost.clear();
    g_npcTrack.clear();
    AcquireSRWLockExclusive(&g_buyLock);   // (nothing left over from a match before)
    g_buysIn.clear();
    g_buysOut.clear();
    ReleaseSRWLockExclusive(&g_buyLock);
    g_free = true;
    g_freeFrom = g_tick;
    if (g_freeRejoin) {   // back in: everyone plays again here, and they hear I'm back
        for (int s = 0; s < g_players && s < 4; ++s) if (s != g_slot) g_gone[s] = false;
        g_freeBackUntil = now + 3000;
        if (!g_liveTick) g_liveTick = g_tick;
        Log(L"Netplay: tick %u - rejoined the match", g_tick);
    }
    SplitViewPredict(false, 0, 0, 0);   // (no input delay to make up for any more)
    Log(L"Netplay: tick %u - the match runs free from here: no lockstep, real clock, my own frame rate", g_tick);
}

void FreeTick() {
    EnterCriticalSection(&g_netLock);
    struct Unlock { ~Unlock() { LeaveCriticalSection(&g_netLock); } } unlock;
    static LARGE_INTEGER lastAt{};
    LARGE_INTEGER now;
    Real_QPC(&now);
    float dt = lastAt.QuadPart ? (float)(now.QuadPart - lastAt.QuadPart) / g_freq.QuadPart : 1.0f / 60;
    if (dt > 0.1f) dt = 0.1f;
    lastAt = now;
    g_tickStartMs = GetTickCount64();
    Receive();
    DirectTick();
    ++g_tick;   // (just a count of my frames now)
    ULONGLONG nowMs = GetTickCount64();
    {
        static ULONGLONG lastBack = 0;   // (rejoined: "I'm back", 10 times a second for a while)
        if (nowMs < g_freeBackUntil && nowMs - lastBack >= 100) {
            lastBack = nowMs;
            PktHeader b{'B', (BYTE)g_slot};
            SendAll(&b, sizeof(b));
        }
    }
    {
        static ULONGLONG lastPing = 0;
        if (nowMs - lastPing >= 250) {
            lastPing = nowMs;
            PktPing ping{{'P', (BYTE)g_slot}, 0, nowMs};
            SendAll(&ping, sizeof(ping));
        }
    }
    Pad mine = SampleLocal(g_tick);
    int ring = g_tick % kRing;
    g_inputs[ring][g_slot] = mine;
    g_have[ring][g_slot] = true;
    g_haveTick[ring][g_slot] = g_tick;
    g_sampledUpTo = g_tick;
    static ULONGLONG lastSend = 0;
    if (nowMs - lastSend >= 8) {   // (each packet has my last 8 frames: 125 a second at most)
        lastSend = nowMs;
        SendInputs(g_tick);
    }
    g_current[g_slot] = mine;
    for (int s = 0; s < g_players && s < 4; ++s) {
        if (s == g_slot) continue;
        if (!g_gone[s] && nowMs - g_remote[s].heardAt > 10000) {
            g_gone[s] = true;
            Log(L"Netplay: free - nothing from player %d for 10 s - they left; their character stays idle", s);
            if (g_inSession && s != 0) OverlayShow((PlayerName(s) + L" left the match").c_str(), 5000);
            if (s == 0 && g_inSession) {
                g_freeHostGone = true;
                OverlayShow(L"Lost the connection to the match - back to the lobby", 8000);
                OnlinePauseForceQuit();
            }
        } else if (g_gone[s] && s == 0 && g_inSession && !g_freeHostGone) {   // the host quit ('L'): the match is over
            g_freeHostGone = true;
            Log(L"Netplay: free - the host left the match - back to the lobby");
            OverlayShow((PlayerName(0) + L" (the host) left the match - back to the lobby").c_str(), 8000);
            OnlinePauseForceQuit();
        }
        g_current[s] = FreeRemotePad(s);
    }
    TakeSessionPresses();
    KbmRaw kbm[4] = {};
    for (int s = 0; s < g_players && s < 4; ++s) kbm[s] = g_current[s].kbm;
    {   // everyone's pause menu open: pause the game (onlinepause.cpp)
        BYTE flags[4] = {};
        bool gone[4] = {};
        for (int s = 0; s < g_players && s < 4; ++s) {
            flags[s] = g_current[s].kbm.flags;
            gone[s] = g_gone[s];
        }
        bool keyboard = false;
        int at = OnlinePauseTick(flags, gone, g_players, &keyboard);
        if (at >= 0 && at < 4) {
            if (keyboard) g_current[at].kbm.keys[VK_ESCAPE >> 3] |= (BYTE)(1 << (VK_ESCAPE & 7));
            else g_current[at].buttons |= XINPUT_GAMEPAD_START;
            kbm[at] = g_current[at].kbm;
        }
    }
    OnlineSettingsApply(kbm, g_players);
    KbmApply(kbm, g_players);
    FreeApplyHits();
    FreeApplySpawns();
    FreeTestHurt();
    FreeApplyPickups();
    FreeSendBuys();
    FreeStats();
    if (g_npcSync) {
        FreeTestKillNpcs();
        FreeSendNpcStates();
        FreeNpcFollow(dt);
    }
    FreeApplyRemotes(dt);
    FreeSendState();
    static int frames = 0, moveFrames = 0;   // (my own input and character, for the log below)
    ++frames;
    {
        bool keyMove = false;   // W A S D or the arrows
        static const int kMoveKeys[] = {'W', 'A', 'S', 'D', VK_LEFT, VK_RIGHT, VK_UP, VK_DOWN};
        for (int vk : kMoveKeys)
            keyMove |= (mine.kbm.keys[vk >> 3] >> (vk & 7) & 1) != 0;
        if (keyMove || abs(mine.lx) > 8000 || abs(mine.ly) > 8000) ++moveFrames;
    }
    static ULONGLONG lastLog = 0;
    if (nowMs - lastLog >= 5000) {   // how far off the others' characters were here (the corrections), every 5 s
        lastLog = nowMs;
        static float lastPos[3];
        static bool havePos = false;
        unsigned char* me = PlayerBySlot(g_slot);
        const float* pos = me ? PlayerPos(me) : nullptr;
        float moved = 0;
        if (pos && havePos) moved = sqrtf((pos[0] - lastPos[0]) * (pos[0] - lastPos[0]) + (pos[2] - lastPos[2]) * (pos[2] - lastPos[2]));
        if (pos) { memcpy(lastPos, pos, sizeof(lastPos)); havePos = true; }
        XINPUT_STATE st{};
        bool pad = MyPadState(&st) == ERROR_SUCCESS;
        Log(L"Netplay: free - me (player %d): moved %.1f in 5 s, movement input in %d of %d frames; %s, controller %s, "
            L"health %.0f", g_slot, moved, moveFrames, frames, KbmIsKeyboardSlot(g_slot) ? L"keyboard / mouse player" :
            L"controller player", pad ? L"connected" : L"not connected (none in any of the 4 controller slots)", me ? PlayerHealth(me) : -1.0f);
        frames = moveFrames = 0;
        for (int s = 0; s < g_players && s < 4; ++s) {
            Remote& r = g_remote[s];
            if (s == g_slot || !r.errCount) continue;
            Log(L"Netplay: free - player %d: %.2f off on average, %.2f at most; %d slides, %d teleports, %d health fixes; ping %ld "
                L"ms; aim %.1f deg off on average, %.1f at most; %d hits left to other PCs, %d decided here", s, r.errSum / r.errCount,
                r.errMax, r.slides, r.teleports, r.healthFixes, g_pingMs[s], r.yawErrSum / r.errCount * 57.2958, r.yawErrMax * 57.2958,
                g_hitsBlocked, g_hitsMine);
            r.errSum = r.yawErrSum = 0;
            r.errMax = r.yawErrMax = 0;
            r.errCount = r.slides = r.teleports = r.healthFixes = 0;
        }
    }
}

// Called on the main thread at the start of every game tick (its first controller poll).
void Tick() {
    if (g_free) { FreeTick(); return; }
    if (g_freeRejoin && !g_freeRejoinHud && HandoffHudUp()) {
        g_freeRejoinHud = g_tick;
        Log(L"Netplay: rejoining - my level is up (tick %u)", g_tick);
    }
    bool freeNow = g_freeRejoin ? g_freeRejoinHud && g_tick >= g_freeRejoinHud + kFreeAfterTicks
                                : g_liveTick && g_tick >= g_liveTick + kFreeAfterTicks;
    if (g_freeWanted && g_inSession && g_sessionDM && !g_catchUp && freeNow) {
        EnterCriticalSection(&g_netLock);
        FreeStart();
        LeaveCriticalSection(&g_netLock);
        FreeTick();
        return;
    }
    LONGLONG paced = PaceTick();
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
    DirectTick();
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
        if (g_syncEvery > 0 && g_playStartTick) {   // (the history: every tick)
            HashPlayers(&g_history[g_tick % kHistory]);
            g_historyTick[g_tick % kHistory] = g_tick;
        }
        if (g_syncEvery > 0 && g_tick % g_syncEvery == 0) {
            int k = (g_tick / g_syncEvery) % 8;
            unsigned h = HashPlayers(&g_hashed[k]);
            g_myHash[k] = h;
            g_myHashTick[k] = g_tick;
            if (g_inSession) g_recHashes.push_back({g_tick, h, g_hashed[k].players, g_hashed[k].rng});
            PktHash p{{'H', (BYTE)g_slot}, g_tick, h, g_hashed[k].players, g_hashed[k].rng};
            if (!g_catchUp || (g_myBackAt && g_tick >= g_myBackAt)) SendAll(&p, sizeof(p));   // (catching up: not yet)
            if (g_catchUp) CheckHostHash(g_tick, g_hashed[k]);
            for (int s = 0; s < g_players; ++s)   // (not with a player who has left: they're idle here from then)
                if (s != g_slot && !g_gone[s] && !g_goneFrom[s] && !g_backAt[s] && g_peerHashTick[s][k] == g_tick)
                    CompareHash(s, g_tick, g_peerHash[s][k], g_peerPlayers[s][k], g_peerRng[s][k]);
            if (g_tick % (g_syncEvery * 10) == 0) {
                LogPlayers(L"check");
                Log(L"Netplay: check tick %u random numbers %08x", g_tick, g_hashed[k].rng);
            }
        }
        {   // a ping to everyone 4 times a second (the round trip: the input delay follows it, the match shows it -
            // FGUYMOD_NetStats)
            static ULONGLONG lastPing = 0;
            ULONGLONG now = GetTickCount64();
            if (now - lastPing >= 250) {
                lastPing = now;
                PktPing ping{{'P', (BYTE)g_slot}, 0, now};
                SendAll(&ping, sizeof(ping));
            }
        }
        g_clockNs += 1000000000LL / g_rate;   // the tick that just ended took 1 / its rate
        ++g_tick;
        BackIn(g_tick);   // (players rejoining from this tick on)
    }
    g_mainBlocksThisTick = 0;
    g_callsThisTick = 0;  // the clock reads exactly base + tick x 1/60 s from here

    // My input goes out for a tick a little ahead (the input delay), so it's everywhere in time. The lobby picks
    // the delay as ticks at the match's top rate; once the rate follows the PCs it's kept as that much time
    // instead (at 100 fps the same 5 ticks were twice as long: the mouse felt slow). Only my own inputs depend on
    // it: a longer delay fills the ticks between with the same input (the mouse's movement counted once), a
    // shorter one samples nothing new until the ticks catch up (the movement waits for the next sample).
    // (catching up: nothing of mine until I'm back in the match - CatchUpBackAt)
    bool sampling = (!g_catchUp || g_myBackAt) && !g_catchUpFailed;
    if (!g_sampledUpTo && !g_catchUp) g_sampledUpTo = (unsigned)g_delay - 1;   // (the first ticks: everyone idle, see Barrier)
    int delayNow = g_delay;
    if (g_rateFollows && g_rateTarget > 0) {
        delayNow = (int)std::ceil(g_delay * (double)g_rate / g_rateTarget - 1e-6);
        if (delayNow < 2) delayNow = 2;
        if (delayNow > 40) delayNow = 40;
    }
    // Online the delay follows the connection: my input must reach everyone before they play its tick - half the
    // worst round trip of the last 2 s, plus room for its ups and downs - at this tick's rate. Up at once (a late
    // input holds everyone up), down only after 3 s of better. The lobby's delay (above) until the first pings are
    // back. (Only my own inputs depend on it: it needn't be the same on every PC.)
    {
        int worst = -1;
        for (int s = 0; s < g_players && s < 4; ++s) {
            if (s == g_slot || Idle(s, g_tick) || g_pingAt[s] == 0) continue;
            for (int k = 0; k < kPingRecent && k < (int)g_pingAt[s]; ++k)   // (over 1.5 s: their game was busy - a load)
                if (g_pingRecent[s][k] > worst && g_pingRecent[s][k] <= 1500) worst = g_pingRecent[s][k];
        }
        static int held = 0;
        static ULONGLONG lowSince = 0;
        // (from the level on: while the games boot and load, a busy game answers late and the pings say nothing)
        if (g_inSession && NetplayLevelLive() && worst >= 0 && g_rate > 0) {
            double needMs = worst * 0.6 + 20;
            int want = (int)std::ceil(needMs * g_rate / 1000.0) + 1;
            if (want < 2) want = 2;
            if (want > 160) want = 160;
            ULONGLONG t = GetTickCount64();
            if (!held) held = delayNow;   // (from the lobby's)
            if (want > held) {
                held = want;
                lowSince = 0;
            } else if (want < held) {   // (a step a tick: the ticks catch up with what's already been sent)
                if (!lowSince) lowSince = t;
                else if (t - lowSince >= 3000) --held;
            } else {
                lowSince = 0;
            }
            delayNow = held;
        }
        static int lastD = 0, logged = 0;
        if (delayNow != lastD && ++logged <= 60)
            Log(L"Netplay: tick %u - my input delay %d ticks (%.0f ms; worst round trip %d ms)", g_tick, delayNow,
                delayNow * 1000.0 / g_rate, worst);
        lastD = delayNow;
    }
    unsigned future = g_tick + delayNow - 1;
    if (sampling && future > g_sampledUpTo) {
        Pad p = SampleLocal(future);
        for (unsigned f = g_sampledUpTo + 1; f <= future; ++f) {
            Pad q = p;
            if (f != future) {   // (a tick in between: the same keys and buttons, the mouse's movement once)
                q.kbm.dx = q.kbm.dy = q.kbm.dz = 0;
                q.kbm.sync = 0;
            }
            int r = f % kRing;
            g_inputs[r][g_slot] = q;
            g_have[r][g_slot] = true;
            g_haveTick[r][g_slot] = f;
        }
        g_sampledUpTo = future;
    }
    future = g_sampledUpTo;
    if (sampling) {
        static ULONGLONG lastSend = 0;   // (catching up, ticks go by fast: not more often than every 5 ms)
        ULONGLONG now = GetTickCount64();
        if (!g_catchUp || now - lastSend >= 5) { SendInputs(future); lastSend = now; }
    }

    LARGE_INTEGER s0, s1, s2;
    Real_QPC(&s0);
    ULONGLONG waitFrom = GetTickCount64(), until = waitFrom + 10000, nextNotice = waitFrom + 400;
    ULONGLONG nextResend = GetTickCount64() + 5, nextAsk = 0;
    bool noticed = false, lostHost = false, fromRecord = false;
    Row row{};
    if (g_catchUp) fromRecord = CatchUpInputs(&row);
    else while (!HaveAll(g_tick)) {
        Receive();
        ULONGLONG now = GetTickCount64();
        if (now >= nextResend) { SendInputs(future); nextResend = now + 5; }
        if (now - waitFrom >= 150 && now >= nextAsk) {   // held up a while: a packet with their inputs may be lost
            nextAsk = now + 100;
            int q = g_tick % kRing;
            for (int s = 0; s < g_players && s < 4; ++s)
                if (s != g_slot && !Idle(s, g_tick) && (!g_have[q][s] || g_haveTick[q][s] != g_tick)) {
                    PktResend ask{{'N', (BYTE)g_slot}, (BYTE)s, g_tick};
                    SendAll(&ask, sizeof(ask));
                }
        }
        if (g_droppedMe) break;   // (they've given up on me: below)
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
                    PktDropped d{{'D', (BYTE)g_slot}, (BYTE)s, g_tick};   // (everyone - them too: they're out)
                    for (int k = 0; k < 4; ++k) SendAll(&d, sizeof(d));
                    if (g_inSession) OverlayShow((PlayerName(s) + L" left the match").c_str(), 5000);
                    if (s == 0 && g_slot != 0) lostHost = true;
                }
            HaveAll(g_tick);
            noticed = false;
            break;
        }
        Real_Sleep(0);
    }
    if (noticed) OverlayHide();
    if (g_droppedMe && g_inSession && !HaveAll(g_tick)) {   // (the others play on without me: their match isn't mine any
        for (int s = 0; s < g_players && s < 4; ++s)        //  more - mine goes on idle until the level quits)
            if (s != g_slot) g_gone[s] = true;
        HaveAll(g_tick);
    }
    if (g_droppedMe == 1 && g_inSession) {
        InterlockedExchange(&g_droppedMe, 2);
        OverlayShow(L"Your connection to the match dropped - back to the lobby, where you can rejoin it", 8000);
        OnlinePauseForceQuit();
    }
    if (lostHost && g_inSession) {   // (a guest without the host: the match can't go on - back to its lobby, to rejoin)
        Log(L"Netplay: lost the connection to the host - back to the lobby (the match can be rejoined from there)");
        OverlayShow(L"Lost the connection to the match - back to the lobby, where you can rejoin it", 8000);
        OnlinePauseForceQuit();
    }
    // This tick's inputs, and who's idle in it: as the host played it (catching up) or as they came. Recorded, for a
    // player who rejoins.
    bool idleNow[4] = {};
    if (fromRecord) {
        for (int s = 0; s < g_players && s < 4; ++s) {
            g_current[s] = row.pads[s];
            idleNow[s] = ((row.idle >> s) & 1) != 0;
            if (s != g_slot) g_gone[s] = idleNow[s];   // (who the host has playing, for once I'm back in the match)
        }
    } else {
        for (int s = 0; s < g_players && s < 4; ++s) {
            g_current[s] = g_inputs[g_tick % kRing][s];
            idleNow[s] = Idle(s, g_tick);
        }
    }
    if (g_inSession) {
        Row rec{};
        for (int s = 0; s < g_players && s < 4; ++s) {
            rec.pads[s] = g_current[s];
            if (idleNow[s]) rec.idle |= (BYTE)(1 << s);
        }
        Record(rec);
    }
    if (g_catchUp) CatchUpProgress();
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
            gone[s] = idleNow[s];
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
    // together again once they've landed - at 60): from here the match's frame rate follows the PCs
    if (!g_rateFollows && g_inSession && g_liveTick && g_tick >= g_liveTick + 600) {
        const int kViewportCount = 0xb53e60;   // (story co-op: once everyone has dropped in)
        if (!g_sessionStory || *reinterpret_cast<volatile int*>(kViewportCount) >= g_players) {
            g_rateFollows = true;
            if (!g_paceMine) g_paceMine = 60;   // (from the rate it loaded at, up from there: NotePace)
            Log(L"Netplay: from tick %u the match runs at up to %d frames per second - as fast as the slowest PC "
                L"manages", g_tick, g_rateTarget);
        }
    }
    if (g_rateFollows) {   // this tick's rate: the slowest PC's (from this tick's inputs - the same on every PC)
        int rate = g_rateTarget;
        bool told = false;   // (no PC has said yet - the first ticks: the rate stays)
        for (int s = 0; s < g_players && s < 4; ++s)
            if (!idleNow[s] && g_current[s].kbm.pace >= 30) {
                told = true;
                if (g_current[s].kbm.pace < rate) rate = g_current[s].kbm.pace;
            }
        if (!told) rate = g_rate;
        if (rate != g_rate) {
            static int logged = 0;
            if (++logged <= 60) Log(L"Netplay: tick %u - %d frames per second", g_tick, rate);
            g_rate = rate;
        }
    }
    if (g_inSession && g_current[0].kbm.sync == 1) RngResync();   // the host's random numbers, everyone
    if (g_inSession && (g_current[0].kbm.sync & 0x10)) RejoinWord(g_current[0].kbm.sync & 3);   // a player back
    if (g_catchUp && g_myBackAt && g_tick >= g_myBackAt + (unsigned)g_delay) CatchUpDone();   // (in step with everyone)
    if (g_inSession) OnlineSettingsApply(kbm, g_players);   // everyone's own aim settings (onlinesettings.cpp)
    if (g_inSession) LoadSyncTickStart();   // menus' movies loaded the same tick everywhere (loadsync.cpp)
    if (g_inSession) SplitViewSpectateTick(g_rate);   // dead in Multiverse Madness: a teammate's view (display only)
    if (g_inSession) CamDump(g_tick);   // research (FGUYMOD_CAMDUMP, rngtrace.cpp)
    if (g_inSession) {   // (the level's start-up must reach this the same tick on every PC - loadsync.cpp)
        static bool viewsOn = false;
        bool on = *reinterpret_cast<volatile DWORD*>(0xb5e468) != 0;   // viewport 0's camera has its controller
        if (on != viewsOn) Log(L"Netplay: the players' views %s at tick %u", on ? L"start" : L"stop", g_tick);
        viewsOn = on;
    }
    if (LONG w = InterlockedExchange(&g_testResW, 0)) {   // (test: what Options > Screen does, on the game thread)
        using ChangeRes_t = void(__cdecl*)(int renderer, int w, int h, int windowed, void* revert);
        Log(L"Netplay: test - changing the resolution to %ldx%ld at tick %u", w, (LONG)g_testResH, g_tick);
        reinterpret_cast<ChangeRes_t>(0x44f920)(*reinterpret_cast<int*>(0xad39e8), w, g_testResH, 1,
                                                reinterpret_cast<void*>(0x4cac40));
    }
    if (InterlockedExchange(&g_testNudge, 0)) {   // (test: this PC's game differs from the others' from here)
        unsigned char* list[8];
        int n = GamePlayers(list, 8);
        for (int i = 0; i < n; ++i)
            if (*reinterpret_cast<int*>(list[i] + 0xdf8) == g_slot) {
                float* pos = const_cast<float*>(PlayerPos(list[i]));
                if (pos) pos[0] += 2.0f;
            }
    }
    {   // test: FGUYMOD_TEST_MONEY=<tick>:<player>:<amount>,... - at that tick, on every PC, that player's match money
        // (the wallet at player +0xbc, its +0x224 - 0x5bf420)
        static int moneys = -1;
        static unsigned moneyTick[8];
        static int moneyPlayer[8], moneyAmount[8];
        if (moneys < 0) {
            moneys = 0;
            char env[128];
            if (GetEnvironmentVariableA("FGUYMOD_TEST_MONEY", env, sizeof(env))) {
                char* ctx = nullptr;
                for (char* t = strtok_s(env, ",", &ctx); t && moneys < 8; t = strtok_s(nullptr, ",", &ctx))
                    if (sscanf_s(t, "%u:%d:%d", &moneyTick[moneys], &moneyPlayer[moneys], &moneyAmount[moneys]) == 3) ++moneys;
            }
        }
        for (int d = 0; d < moneys; ++d)
            if (g_inSession && g_tick == moneyTick[d]) {
                unsigned char* list[8];
                int n = GamePlayers(list, 8);
                for (int i = 0; i < n; ++i)
                    if (*reinterpret_cast<int*>(list[i] + 0xdf8) == moneyPlayer[d]) {
                        int* money = reinterpret_cast<int*>(list[i] + 0xbc + 0x224);
                        Log(L"Netplay: test - player %d's money %d -> %d at tick %u", moneyPlayer[d], *money, moneyAmount[d], g_tick);
                        *money = moneyAmount[d];
                    }
            }
    }
    {   // test: FGUYMOD_TEST_MEDAL=<tick>:<player>:<medal>,... - at that tick, on every PC, that player earns that medal
        // (0x9abfb0 on the HUD manager 0x97e760: its money is paid by 0x9a4fa0 - loadsync.cpp)
        static int medals = -1;
        static unsigned medalTick[8];
        static int medalPlayer[8], medalId[8];
        if (medals < 0) {
            medals = 0;
            char env[128];
            if (GetEnvironmentVariableA("FGUYMOD_TEST_MEDAL", env, sizeof(env))) {
                char* ctx = nullptr;
                for (char* t = strtok_s(env, ",", &ctx); t && medals < 8; t = strtok_s(nullptr, ",", &ctx))
                    if (sscanf_s(t, "%u:%d:%d", &medalTick[medals], &medalPlayer[medals], &medalId[medals]) == 3) ++medals;
            }
        }
        for (int d = 0; d < medals; ++d)
            if (g_inSession && g_tick == medalTick[d]) {
                using Hud_t = void*(__cdecl*)();
                using Medal_t = void(__thiscall*)(void*, int, int);
                Log(L"Netplay: test - player %d earns medal %d at tick %u", medalPlayer[d], medalId[d], g_tick);
                reinterpret_cast<Medal_t>(0x9abfb0)(reinterpret_cast<Hud_t>(0x97e760)(), medalId[d], medalPlayer[d]);
            }
    }
    {   // test: FGUYMOD_TEST_DROP=<tick>:<player>,... - at that tick, on every PC, that player is dropped far below
        // the level (falls out, dies, respawns: the respawn must come out the same everywhere)
        static int drops = -1;
        static unsigned dropTick[8];
        static int dropPlayer[8];
        if (drops < 0) {
            drops = 0;
            char env[128];
            if (GetEnvironmentVariableA("FGUYMOD_TEST_DROP", env, sizeof(env))) {
                char* ctx = nullptr;
                for (char* t = strtok_s(env, ",", &ctx); t && drops < 8; t = strtok_s(nullptr, ",", &ctx))
                    if (sscanf_s(t, "%u:%d", &dropTick[drops], &dropPlayer[drops]) == 2) ++drops;
            }
        }
        for (int d = 0; d < drops; ++d)
            if (g_inSession && g_tick == dropTick[d]) {
                unsigned char* list[8];
                int n = GamePlayers(list, 8);
                for (int i = 0; i < n; ++i)
                    if (*reinterpret_cast<int*>(list[i] + 0xdf8) == dropPlayer[d]) {
                        float* pos = const_cast<float*>(PlayerPos(list[i]));
                        if (pos) pos[1] -= 3000.0f;
                        Log(L"Netplay: test - player %d dropped out of the level at tick %u (position at %p)", dropPlayer[d], g_tick, pos);
                    }
            }
    }
    {   // test: FGUYMOD_TEST_RNGDRIFT=<tick> - there the guest (slot 1) takes one random number more than the others
        // (what a look or effect on one PC only did): the safety net must bring everyone back together
        static int driftAt = -1;
        if (driftAt < 0) {
            char env[16];
            driftAt = GetEnvironmentVariableA("FGUYMOD_TEST_RNGDRIFT", env, sizeof(env)) ? atoi(env) : 0;
        }
        if (driftAt > 0 && g_inSession && g_slot == 1 && g_tick == (unsigned)driftAt) {
            reinterpret_cast<float(__cdecl*)()>(0x4f69a0)();
            Log(L"Netplay: test - took one random number more than the others at tick %u", g_tick);
        }
    }
    KbmApply(kbm, g_players);  // keyboard / mouse players' devices, from their raw input (kbm.cpp)
    if (g_inSession) PredictMyView();   // my view turns at once on my screen (splitview.cpp; display only)
    Real_QPC(&s1);
    {   // this PC's own pace: a quarter second's worth of ticks, minus the waiting (for the others' inputs, and
        // for the tick's time to come round)
        static LARGE_INTEGER windowStart{};
        static LONGLONG windowWait = 0;
        static int windowTicks = 0;
        if (!windowStart.QuadPart) windowStart = s0;
        windowWait += s1.QuadPart - s0.QuadPart + paced;
        if (++windowTicks >= (g_rate >= 60 ? g_rate / 4 : 15)) {
            LONGLONG work = s1.QuadPart - windowStart.QuadPart - windowWait;
            if (work > 0) NotePace((double)windowTicks * g_freq.QuadPart / work);
            windowStart = s1;
            windowWait = 0;
            windowTicks = 0;
        }
    }
    WaitForQuiescence();
    Real_QPC(&s2);
    g_statPeer += s1.QuadPart - s0.QuadPart;
    g_statQuiet += s2.QuadPart - s1.QuadPart;

    if (g_traceFrom && g_tick == g_traceFrom) g_traceOn = true;
    if (g_traceFrom && g_tick == g_traceFrom + 4 && g_traceOn) { g_traceOn = false; TraceDump(); }
    Trace(8, g_tick);
    if (g_tick % 600 == 0) {
        static ULONGLONG last = GetTickCount64();
        static LONGLONG lastGame = 0;
        ULONGLONG now = GetTickCount64();
        LONGLONG game = TickTime(1000);
        Log(L"Netplay: tick %u (%.1f ticks/s over the last 600; game time %.2f s in %.2f s; per tick: %.2f ms waiting "
            L"for peers, %.2f ms for threads, %d timeouts, %lld clock reads)", g_tick, 600000.0 / (now - last + 1),
            (game - lastGame) / 1000.0, (now - last) / 1000.0, g_statPeer * 1000.0 / g_freq.QuadPart / 600,
            g_statQuiet * 1000.0 / g_freq.QuadPart / 600, g_statTimeouts, g_statQpcCalls / 600);
        last = now;
        lastGame = game;
        g_statPeer = g_statQuiet = g_statQpcCalls = 0;
        g_statTimeouts = 0;
    }
}

}  // namespace

// Called by vpad.cpp's XInputGetState hook. Returns true if netplay supplies this pad's state.
bool NetplayPad(DWORD pad, XINPUT_STATE* state, DWORD(WINAPI* real)(DWORD, XINPUT_STATE*)) {
    if (!g_enabled) {
        // Left the session (back to the lobby): its players' controllers still read as plugged in - idle - so
        // the game doesn't stop for "reconnect the controller" (it did, mid-quit). Mine: the real one if there.
        if (!g_left || pad >= 4 || (int)pad >= g_players) return false;
        if (pad == 0 && real && real(0, state) == ERROR_SUCCESS) return false;
        ZeroMemory(state, sizeof(*state));
        return true;
    }
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
bool NetplayPadConnected(DWORD pad) { return (g_enabled || g_left) && pad < 4 && (int)pad < g_players; }

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
bool NetplayFreeRun() { return g_enabled && g_free; }

// The store (ui.cpp): may this purchase for that player go through here? (Free-running: only my own player's.)
bool NetplayStoreAllowed(int player) { return !NetplayFreeRun() || player == g_slot; }
void NetplayStoreBought(int player, int weapon) {
    if (!NetplayFreeRun() || player != g_slot) return;
    AcquireSRWLockExclusive(&g_buyLock);
    g_buysOut.push_back(weapon);
    ReleaseSRWLockExclusive(&g_buyLock);
}
bool NetplayStoreTake(int* player, int* weapon) {
    AcquireSRWLockExclusive(&g_buyLock);
    bool any = !g_buysIn.empty();
    if (any) {
        *player = g_buysIn.front().first;
        *weapon = g_buysIn.front().second;
        g_buysIn.erase(g_buysIn.begin());
    }
    ReleaseSRWLockExclusive(&g_buyLock);
    return any;
}

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
        if (!g_free) {   // (a free-running match is on the shifted real clock already)
            LARGE_INTEGER real;
            Real_QPC(&real);
            g_qpcOffset = kClockBaseSeconds * g_freq.QuadPart + TickTime(g_freq.QuadPart) +
                          (++g_callsThisTick) - real.QuadPart;
            g_tickCountOffset = (LONGLONG)FixedMs() - (LONGLONG)Real_GetTickCount();
            g_timeGetTimeOffset = (LONGLONG)FixedMs() - (LONGLONG)Real_timeGetTime();
        }
        g_left = true;
        g_free = false;      // (the game's own damage again)
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
const wchar_t* NetplayLobbyName(int slot) {   // a player's lobby name, or "" (no lobby: a LAN / test session)
    ReadNames();
    return slot >= 0 && slot < 4 ? g_names[slot] : L"";
}
void NetplayTestResolution(int w, int h) { InterlockedExchange(&g_testResH, h); InterlockedExchange(&g_testResW, w); }
void NetplayTestHurt(int player, float amount) {
    g_testHurtAmount = amount;
    InterlockedExchange(&g_testHurtWho, player);
}
void NetplayTestKillNpcs() { InterlockedExchange(&g_testKillNpcs, 1); }
void NetplayTestNudge() { InterlockedExchange(&g_testNudge, 1); Log(L"Netplay: test - nudging my player on this PC"); }
void NetplayTestStall(DWORD ms) {
    g_testStallUntil = GetTickCount64() + ms;
    Log(L"Netplay: test - sending nothing for %lu ms", ms);
}
void NetplayTestLeave() {
    Log(L"Netplay: test - quitting the match to the lobby");
    OnlinePauseForceQuit();
}
int NetplaySessionSlot() { return g_slot; }
DWORD NetplayGameThread() { return g_mainThread; }
void NetplayWatchAim(int player) {
    unsigned char* players[8];
    int n = GamePlayers(players, 8);
    for (int i = 0; i < n; ++i)
        if (*reinterpret_cast<int*>(players[i] + 0xdf8) == player) {
            if (const float* aim = PlayerAim(players[i])) {
                wchar_t what[64];
                swprintf_s(what, L"player %d's aim height", player);
                WatchAddress(aim + 1, what);
            }
            return;
        }
    Log(L"Watch: no player %d", player);
}
unsigned NetplayTick() { return g_tick; }
int NetplayRate() { return g_rate; }
// Research (test pipe "profile <ms>"): where the render thread spends its time - its code address sampled every ms,
// the most common places (256-byte buckets) into the log.
void NetplayProfileRender(DWORD ms) {
    DWORD tid = g_renderThread;
    HANDLE h = tid ? OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, tid) : nullptr;
    if (!h) { Log(L"Profile: no render thread"); return; }
    std::map<ULONG_PTR, int> hits;
    int samples = 0;
    ULONGLONG until = GetTickCount64() + ms;
    while (GetTickCount64() < until) {
        if (SuspendThread(h) != (DWORD)-1) {
            CONTEXT ctx{};
            ctx.ContextFlags = CONTEXT_CONTROL;
            if (GetThreadContext(h, &ctx)) { ++hits[ctx.Eip & ~0xffu]; ++samples; }
            ResumeThread(h);
        }
        Real_Sleep(1);
    }
    CloseHandle(h);
    std::vector<std::pair<int, ULONG_PTR>> top;
    for (auto& e : hits) top.push_back({e.second, e.first});
    std::sort(top.rbegin(), top.rend());
    Log(L"Profile: render thread, %d samples", samples);
    for (size_t i = 0; i < top.size() && i < 30; ++i) {
        HMODULE m = nullptr;
        wchar_t name[MAX_PATH] = L"?";
        if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                               (LPCWSTR)top[i].second, &m) && m)
            GetModuleFileNameW(m, name, MAX_PATH);
        const wchar_t* base = wcsrchr(name, L'\\');
        Log(L"Profile: %5.1f%%  %s+%lx", top[i].first * 100.0 / samples, base ? base + 1 : name,
            (unsigned long)(top[i].second - (ULONG_PTR)m));
    }
}

void NetplayLogStacks() {
    Log(L"Netplay: game thread %lu:", g_mainThread);
    LogStack(g_mainThread);
    if (g_renderThread) { Log(L"Netplay: render thread %lu:", g_renderThread); LogStack(g_renderThread); }
}
// splitview.cpp (my view turning at once): the tick the frame being drawn shows, and the model's turn of my inputs of
// the ticks after `from` up to `to`.
unsigned NetplayShownTick() { return (unsigned)g_shownTick; }
float NetplayModelTurn(unsigned from, unsigned to) {
    float sum = 0;
    for (unsigned t = from + 1; t <= to && to - t < kRing / 2; ++t) sum += g_turnByTick[t % kRing];
    return sum;
}
bool NetplayLevelLive() { return g_enabled && g_liveTick && g_tick >= g_liveTick; }
bool NetplayStoryCoop() { return g_enabled && g_sessionStory; }
float NetplayPlayerHealth(const unsigned char* player) { return PlayerHealth(player); }

// modloader.cpp (plugins): a player's input for the tick being run - the match's synced input, the same on every PC.
bool NetplayInputOf(int slot, WORD* buttons, BYTE* lt, BYTE* rt, SHORT* sticks, KbmRaw* kbm, bool* keyboard) {
    if (!g_enabled || slot < 0 || slot >= g_players || slot >= 4) return false;
    const Pad& p = g_current[slot];
    *buttons = p.buttons;
    *lt = p.lt;
    *rt = p.rt;
    sticks[0] = p.lx; sticks[1] = p.ly; sticks[2] = p.rx; sticks[3] = p.ry;
    *kbm = p.kbm;
    *keyboard = KbmIsKeyboardSlot(slot);
    return true;
}

// FGUYMOD_NetStats: the round trip to every other player still in the match - "name:ms|..." (ms -1: not known yet).
std::string NetplayStats() {
    std::string out;
    if (!g_enabled || !g_inSession) return out;
    for (int s = 0; s < g_players && s < 4; ++s) {
        if (s == g_slot || g_gone[s] || g_goneFrom[s]) continue;
        char part[96];
        std::wstring name = PlayerName(s);
        char n8[64];
        WideCharToMultiByte(CP_UTF8, 0, name.c_str(), -1, n8, sizeof(n8), nullptr, nullptr);
        for (char* c = n8; *c; ++c) if (*c == '|' || *c == ':') *c = ' ';
        sprintf_s(part, "%s%s:%ld", out.empty() ? "" : "|", n8, g_pingMs[s]);
        out += part;
    }
    return out;
}

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
    g_syncEvery = SettingInt(L"Netplay", L"SyncCheckFrames", L"FGUYMOD_TEST_SYNCEVERY", 30);
    g_allClock = SettingInt(L"Netplay", L"_", L"FGUYMOD_TEST_ALLCLOCK", 0) != 0;
    g_freeWanted = SettingInt(L"Netplay", L"_", L"FGUYMOD_NET_FREE", 0) != 0;   // (being built: every copy alike)
    g_freeRejoin = g_freeWanted && SettingInt(L"Netplay", L"_", L"FGUYMOD_NET_REJOIN", 0) != 0;
    InitializeCriticalSection(&g_hitLock);
    if (g_freeWanted) {   // the players' TakeDamage (vtable slot 58): who decides a hit, once the match runs free
        auto slot = reinterpret_cast<void**>(kPlayerVtable) + 58;
        DWORD old;
        VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &old);
        Real_TakeDamage = reinterpret_cast<TakeDamage_t>(*slot);
        *slot = reinterpret_cast<void*>(&Free_TakeDamage);
        VirtualProtect(slot, sizeof(void*), old, &old);
        Log(L"Netplay: the match will run free (no lockstep) once everyone's level is up");
        if (InstallPickupHooks()) Log(L"Netplay: pickups shared (only my own player takes them here)");
        g_npcLog = SettingInt(L"Debug", L"_", L"FGUYMOD_TEST_NPCLOG", 0);
        g_npcBlock = SettingInt(L"Debug", L"_", L"FGUYMOD_TEST_NPCBLOCK", 0);
        g_npcSync = SettingInt(L"Debug", L"_", L"FGUYMOD_TEST_NPCSYNC", 0);
        if (g_npcSync && InstallNpcSpawnHook() && InstallEntityDamageHook())
            Log(L"Netplay: being built - the host's game spawns the enemies, their state and damage the host's");
        if ((g_npcLog || g_npcBlock) && InstallNpcHooks())
            Log(L"Netplay: research - enemies going in watched%s", g_npcBlock ? L", the others' PCs place none" : L"");
    }
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
    g_sessionDM = sessionLen > 0 && sessionLen < sizeof(session) && strncmp(session, "DM,", 3) == 0;
    {   // (research: FGUYMOD_TEST_NETFREE=all - the free-running match in any mode, to look at its enemies)
        char all[8] = "";
        if (GetEnvironmentVariableA("FGUYMOD_TEST_NETFREE", all, sizeof(all)) && strcmp(all, "all") == 0) g_sessionDM = true;
    }
    g_inSession = sessionLen > 0 && sessionLen < sizeof(session);
    KbmSessionStart(g_inSession ? session : nullptr);
    if (g_inSession) OnlineSettingsInit();   // my own settings (onlinesettings.cpp)
    // rejoining a running match (online.cpp starts this game so): catch up with it first
    g_catchUp = g_inSession && SettingInt(L"Netplay", L"_", L"FGUYMOD_NET_CATCHUP", 0) != 0;
    if (g_catchUp) {
        wchar_t name[64];
        swprintf_s(name, L"Local\\FGUYMod_CatchUp_%lu", GetCurrentProcessId());
        HANDLE map = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, sizeof(CatchUpShared), name);
        if (map) g_cuShared = static_cast<CatchUpShared*>(MapViewOfFile(map, FILE_MAP_WRITE, 0, 0, sizeof(CatchUpShared)));
        Log(L"Netplay: this game rejoins a running match");
    }
    g_phase = Phase::Boot;
    Log(L"Netplay: up to %d frames per second in the match (60 until everyone is in the level)", g_rateTarget);
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
