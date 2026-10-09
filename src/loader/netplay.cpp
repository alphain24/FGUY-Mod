// Netplay core: lockstep input sync.
//
// Every peer runs the same game. Each peer owns one player slot (= one virtual XInput pad). Every
// frame, each peer sends its pad state for (frame + InputDelay) to the others and waits until it has
// every slot's state for the current frame, so all peers feed identical inputs into identical frames.
//
// For that to stay in sync, the game must behave identically on every PC:
//   * Lockstep runs from the game's very first frame (all peers meet at the first Present), so menus
//     and loading are lockstepped too and there is no free-running time before the match.
//   * The game's main thread sees a fixed clock from the moment we load: exactly 1/60 s per frame,
//     the same absolute values on every PC (QPC, timeGetTime, GetTickCount).
//   * The C runtime random generator gets a fixed seed.
//   * Gameplay input is read by the game's pad thread, which the main thread wakes at the start of
//     each frame (SetEvent) and which then polls the pads concurrently with the frame. We make the
//     main thread wait right there until that pass is done, so the frame always reads its own inputs.
//   * Local keyboard / mouse / cursor don't reach the game (dinput.cpp, Hook_GetCursorPos).
//   * Background work (level streaming etc.) finishes on the same frame everywhere: file reads are
//     completed before ReadFile returns, and at the end of every frame we wait until all of the game's
//     other threads are parked in a wait/sleep ("quiescence").
// Every SyncCheckFrames frames peers compare a hash of the players' positions (desync detector).
//
// Settings ([Netplay] in FGUYMod.ini, env overrides for tests):
//   Enable=1  Slot=0..3  Players=2  Port=7600  Peers=127.0.0.1:7601,...  InputDelay=3
//   Script=1 (scripted test inputs instead of the real pad)  SyncCheckFrames=60

#include "common.h"

#include <winsock2.h>
#include <ws2tcpip.h>
#include <xinput.h>

#include <cstdio>
#include <cstring>
#include <vector>

#pragma comment(lib, "ws2_32.lib")

namespace {

// ---- settings ---------------------------------------------------------------------------------------
bool g_enabled = false;
int g_slot = 0, g_players = 2, g_delay = 3, g_syncEvery = 60;
bool g_script = false;
int g_frameWaitMs = 0;        // research: extra real time per frame (FGUYMOD_NET_FRAMEWAIT)
bool g_idleScript = false;    // research: scripted input that only leaves the start pause (FGUYMOD_NET_IDLE)
std::vector<sockaddr_in> g_peers;

// ---- state ------------------------------------------------------------------------------------------
struct Pad {
    WORD buttons;
    BYTE lt, rt;
    SHORT lx, ly, rx, ry;
};
const int kRing = 256;               // frames of input history kept
Pad g_inputs[kRing][4];
bool g_have[kRing][4];
unsigned g_haveFrame[kRing][4];      // which frame each ring entry holds
Pad g_current[4];                    // what XInputGetState reports this frame

enum class Phase { Off, Boot, Running };
Phase g_phase = Phase::Off;
unsigned g_frame = 0;                // lockstep frame number (frame being simulated)
bool g_peerReady[4];
DWORD g_mainThread = 0;              // the thread that loaded us = the game's main / render thread
SOCKET g_sock = INVALID_SOCKET;

// ---- fixed clock --------------------------------------------------------------------------------------
LARGE_INTEGER g_freq;
const LONGLONG kClockBaseSeconds = 1000;  // same absolute time on every PC
LONGLONG g_callsThisFrame = 0;
decltype(&QueryPerformanceCounter) Real_QPC;
decltype(&GetTickCount) Real_GetTickCount;
using timeGetTime_t = DWORD(WINAPI*)();
timeGetTime_t Real_timeGetTime;
using srand_t = void(__cdecl*)(unsigned);
srand_t Real_srand;

bool FixedClock() { return g_phase != Phase::Off && GetCurrentThreadId() == g_mainThread; }

BOOL WINAPI Hook_QPC(LARGE_INTEGER* out) {
    if (!FixedClock()) return Real_QPC(out);
    // 1/60 s per frame, plus one tick per call so "wait until time passes" loops still finish.
    out->QuadPart = kClockBaseSeconds * g_freq.QuadPart + (LONGLONG)g_frame * g_freq.QuadPart / 60 +
                    (++g_callsThisFrame);
    return TRUE;
}
DWORD FixedMs() { return (DWORD)(kClockBaseSeconds * 1000 + (ULONGLONG)g_frame * 1000 / 60); }
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

// ---- the game's pad thread ----------------------------------------------------------------------------
// It waits on an event; the main thread sets that event right after Present and the pad thread then
// polls all input devices (~19 polls, ~0.02 ms) and stores the results, while the main thread carries
// on with its frame. We learn which thread and which event it is, and when the main thread sets that
// event, hold the main thread until the pass is done.
volatile DWORD g_padThread = 0;
volatile HANDLE g_padEvent = nullptr;
volatile LONG g_padWakes = 0;        // times the pad thread's wait returned
volatile LONG g_padWaiting = 0;      // pad thread is inside its wait right now
LONG g_padSyncs = 0, g_padSyncTimeouts = 0;
decltype(&WaitForSingleObject) Real_WaitForSingleObject;
decltype(&SetEvent) Real_SetEvent;

// Quiescence: every game thread that ever waits/sleeps through the exe's imports is tracked here.
struct ThreadState { volatile DWORD tid; volatile LONG idle; volatile LONG busySince; };
ThreadState g_threads[64];
volatile LONG g_threadCount = 0;

ThreadState* Me() {
    DWORD me = GetCurrentThreadId();
    LONG n = g_threadCount;
    for (LONG i = 0; i < n; ++i)
        if (g_threads[i].tid == me) return &g_threads[i];
    LONG i = InterlockedIncrement(&g_threadCount) - 1;
    if (i >= 64) { InterlockedDecrement(&g_threadCount); return nullptr; }
    g_threads[i].tid = me;
    return &g_threads[i];
}
struct IdleScope {  // marks the calling thread idle while it blocks
    ThreadState* t;
    IdleScope() : t(GetCurrentThreadId() == g_mainThread ? nullptr : Me()) { if (t) InterlockedExchange(&t->idle, 1); }
    ~IdleScope() { if (t) InterlockedExchange(&t->idle, 0); }
};

void PadDiag(const wchar_t* what, ULONG_PTR a, DWORD ms) {
    static volatile LONG n = 0;
    if (g_padThread && GetCurrentThreadId() == g_padThread && InterlockedIncrement(&n) <= 12)
        Log(L"Netplay diag: pad thread %s(%p, %lu)", what, (void*)a, ms);
}

DWORD WINAPI Hook_WaitForSingleObject(HANDLE h, DWORD ms) {
    PadDiag(L"WaitForSingleObject", (ULONG_PTR)h, ms);
    IdleScope idle;
    if (GetCurrentThreadId() != g_padThread) return Real_WaitForSingleObject(h, ms);
    g_padEvent = h;
    InterlockedExchange(&g_padWaiting, 1);
    DWORD r = Real_WaitForSingleObject(h, ms);
    InterlockedExchange(&g_padWaiting, 0);
    InterlockedIncrement(&g_padWakes);
    return r;
}
decltype(&Sleep) Real_Sleep;
decltype(&SleepEx) Real_SleepEx;
void WINAPI Hook_Sleep(DWORD ms) { PadDiag(L"Sleep", 0, ms); IdleScope idle; Real_Sleep(ms); }
DWORD WINAPI Hook_SleepEx(DWORD ms, BOOL alertable) { PadDiag(L"SleepEx", 0, ms); IdleScope idle; return Real_SleepEx(ms, alertable); }

// Background file reads: finish them before ReadFile returns, so disk timing can't shift a frame.
decltype(&ReadFile) Real_ReadFile;
BOOL WINAPI Hook_ReadFile(HANDLE f, LPVOID buf, DWORD n, LPDWORD read, LPOVERLAPPED ov) {
    BOOL r = Real_ReadFile(f, buf, n, read, ov);
    if (r || !ov || GetLastError() != ERROR_IO_PENDING) return r;
    DWORD got = 0;
    if (!GetOverlappedResult(f, ov, &got, TRUE)) return FALSE;
    if (ov->hEvent) SetEvent(ov->hEvent);  // stays signalled for the game's own wait
    if (read) *read = got;
    return TRUE;
}

void WaitForQuiescence() {
    // All tracked threads parked in a wait or sleep = everything started this frame is done.
    LARGE_INTEGER t0, t;
    Real_QPC(&t0);
    for (;;) {
        bool all = true;
        LONG n = g_threadCount;
        int busy = -1;
        for (LONG i = 0; i < n && all; ++i)
            if (!g_threads[i].idle) { all = false; busy = i; }
        if (all) return;
        Real_QPC(&t);
        if (t.QuadPart - t0.QuadPart > g_freq.QuadPart / 20) {  // 50 ms: don't stall forever
            static int warned = 0;
            if (warned++ < 10) Log(L"Netplay: thread %lu stayed busy at the end of frame %u", g_threads[busy].tid, g_frame);
            return;
        }
        YieldProcessor();
    }
}

BOOL WINAPI Hook_SetEvent(HANDLE h);
decltype(&ReleaseSemaphore) Real_ReleaseSemaphore;
void SyncPadPass(LONG wakes);

BOOL WINAPI Hook_ReleaseSemaphore(HANDLE h, LONG count, LONG* previous) {
    {
        static volatile LONG n = 0;
        if (g_padEvent && InterlockedIncrement(&n) <= 6)
            Log(L"Netplay diag: thread %lu ReleaseSemaphore(%p) (pad waits on %p)", GetCurrentThreadId(), h, g_padEvent);
    }
    bool sync = g_phase == Phase::Running && h && h == g_padEvent && GetCurrentThreadId() == g_mainThread;
    LONG wakes = g_padWakes;
    BOOL r = Real_ReleaseSemaphore(h, count, previous);
    if (sync) SyncPadPass(wakes);
    return r;
}

BOOL WINAPI Hook_SetEvent(HANDLE h) {
    bool sync = g_phase == Phase::Running && h && h == g_padEvent && GetCurrentThreadId() == g_mainThread;
    LONG wakes = g_padWakes;
    BOOL r = Real_SetEvent(h);
    if (sync) SyncPadPass(wakes);
    return r;
}

void SyncPadPass(LONG wakes) {
    {
        // Wait until the pad thread has woken up, run its pass and gone back to waiting.
        LARGE_INTEGER t0, t;
        Real_QPC(&t0);
        for (;;) {
            if (g_padWakes != wakes && g_padWaiting) break;
            Real_QPC(&t);
            if (t.QuadPart - t0.QuadPart > g_freq.QuadPart / 50) { ++g_padSyncTimeouts; break; }  // 20 ms
            YieldProcessor();
        }
        ++g_padSyncs;
    }
}

// ---- packets ------------------------------------------------------------------------------------------
// All little-endian. 'R' ready: slot. 'I' inputs: slot, first frame, count, Pad[count]. 'H' hash: slot,
// frame, hash.
#pragma pack(push, 1)
struct PktHeader { char type; BYTE slot; };
struct PktInputs { PktHeader h; unsigned first; BYTE count; Pad pads[16]; };
struct PktHash { PktHeader h; unsigned frame; unsigned hash; };
#pragma pack(pop)

void SendAll(const void* data, int len) {
    for (auto& p : g_peers) sendto(g_sock, (const char*)data, len, 0, (const sockaddr*)&p, sizeof(p));
}

unsigned g_peerHash[4][8];           // recent hashes from peers, by (frame / syncEvery) % 8
unsigned g_peerHashFrame[4][8];
unsigned g_myHash[8], g_myHashFrame[8];
int g_desyncs = 0;

void LogPlayers(const wchar_t* tag);

void CompareHash(int slot, unsigned frame, unsigned hash) {
    int k = (frame / g_syncEvery) % 8;
    if (g_myHashFrame[k] != frame) return;
    if (g_myHash[k] == hash) {
        if (frame % (g_syncEvery * 10) == 0) Log(L"Netplay: frame %u in sync with peer %d (%08x)", frame, slot, hash);
    } else {
        ++g_desyncs;
        Log(L"Netplay: DESYNC at frame %u: mine %08x, peer %d has %08x", frame, g_myHash[k], slot, hash);
        if (g_desyncs == 1) LogPlayers(L"first desync, now at");
    }
}

void Receive() {
    char buf[1500];
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
                if (f + kRing / 2 < g_frame) continue;  // far too old
                int r = f % kRing;
                g_inputs[r][slot] = p->pads[i];
                g_have[r][slot] = true;
                g_haveFrame[r][slot] = f;
            }
        } else if (h->type == 'H' && n >= (int)sizeof(PktHash)) {
            auto p = reinterpret_cast<PktHash*>(buf);
            int k = (p->frame / g_syncEvery) % 8;
            g_peerHash[slot][k] = p->hash;
            g_peerHashFrame[slot][k] = p->frame;
            CompareHash(slot, p->frame, p->hash);
        }
    }
}

bool HaveAll(unsigned frame) {
    int r = frame % kRing;
    for (int s = 0; s < g_players; ++s)
        if (!g_have[r][s] || g_haveFrame[r][s] != frame) return false;
    return true;
}

void SendInputs(unsigned upTo) {
    PktInputs p{};
    p.h = {'I', (BYTE)g_slot};
    unsigned first = upTo >= 15 ? upTo - 15 : 0;  // resend the last 16 frames (packet loss)
    p.first = first;
    p.count = (BYTE)(upTo - first + 1);
    for (unsigned f = first; f <= upTo; ++f) p.pads[f - first] = g_inputs[f % kRing][g_slot];
    SendAll(&p, (int)offsetof(PktInputs, pads) + p.count * (int)sizeof(Pad));
}

// ---- local input ----------------------------------------------------------------------------------------
using XInputGetState_t = DWORD(WINAPI*)(DWORD, XINPUT_STATE*);
XInputGetState_t g_realXInput = nullptr;
unsigned g_playStartFrame = 0;       // test script: first frame with players in the level

Pad ScriptPad(unsigned f) {
    // A fixed little routine, timed from when the level's players exist: (slot 0) press A to leave the
    // pause menu the level starts in, (slot 1) press Start to drop in, then walk around and jump/attack.
    Pad p{};
    if (!g_playStartFrame || f < g_playStartFrame) return p;
    f -= g_playStartFrame;
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
    return p;
}

// ---- state hash -------------------------------------------------------------------------------------------
unsigned HashPlayers() {
    unsigned h = 2166136261u;
    void* players[8];
    int n = ResearchPlayers(players, 8);
    for (int i = 0; i < n; ++i) {
        auto self = static_cast<unsigned char*>(players[i]);
        if (IsBadReadPtr(self, 0xe00)) continue;
        auto xform = *reinterpret_cast<unsigned char**>(self + 0x38);
        if (IsBadReadPtr(xform, 0x40)) continue;
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
        if (IsBadReadPtr(self, 0xe00)) continue;
        auto xform = *reinterpret_cast<float**>(self + 0x38);
        if (IsBadReadPtr(xform, 0x40)) continue;
        Log(L"Netplay: %s frame %u player %d at (%.4f, %.4f, %.4f)", tag, g_frame,
            *reinterpret_cast<int*>(self + 0xdf8), xform[12], xform[13], xform[14]);
    }
}

// ---- frame boundary ---------------------------------------------------------------------------------------
void Barrier() {
    // Hold the game's first frame until every peer has reached it.
    Log(L"Netplay: first frame - waiting for the other players");
    g_peerReady[g_slot] = true;
    PktHeader r{'R', (BYTE)g_slot};
    ULONGLONG until = GetTickCount64() + 120000;
    for (;;) {
        SendAll(&r, sizeof(r));
        Receive();
        bool all = true;
        for (int s = 0; s < g_players; ++s) all &= g_peerReady[s];
        if (all || GetTickCount64() > until) break;
        Sleep(10);
    }
    for (int i = 0; i < 10; ++i) { SendAll(&r, sizeof(r)); Sleep(5); }  // let late peers see us
    for (unsigned f = 0; f < (unsigned)g_delay; ++f)          // the first frames: everyone idle
        for (int s = 0; s < g_players; ++s) {
            g_inputs[f % kRing][s] = Pad{};
            g_have[f % kRing][s] = true;
            g_haveFrame[f % kRing][s] = f;
        }
    g_phase = Phase::Running;
    Log(L"Netplay: all %d players ready - lockstep from the first frame (slot %d, input delay %d frames)",
        g_players, g_slot, g_delay);
}

}  // namespace

// Called by vpad.cpp's XInputGetState hook. Returns true if netplay supplies this pad's state.
bool NetplayPad(DWORD pad, XINPUT_STATE* state, DWORD(WINAPI* real)(DWORD, XINPUT_STATE*)) {
    if (!g_enabled) return false;
    g_realXInput = real;
    if (!g_padThread && GetCurrentThreadId() != g_mainThread) {
        g_padThread = GetCurrentThreadId();
        Log(L"Netplay: the game's pad thread is %lu", g_padThread);
    }
    if (pad >= 4 || (int)pad >= g_players) return true;  // unused slots: report "not connected"
    static DWORD packet[4];
    static Pad last[4];
    Pad p = g_phase == Phase::Running ? g_current[pad] : Pad{};
    if (memcmp(&p, &last[pad], sizeof(p)) != 0) { last[pad] = p; ++packet[pad]; }
    ZeroMemory(state, sizeof(*state));
    state->dwPacketNumber = packet[pad];
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

// Called at the end of every Present (one rendered frame = one game frame).
void NetplayOnFrame() {
    if (!g_enabled) return;
    Receive();
    if (g_phase == Phase::Boot) Barrier();

    // Frame g_frame has just been simulated and shown.
    if (!g_playStartFrame) {
        void* players[8];
        if (ResearchPlayers(players, 8) > 0) {
            g_playStartFrame = g_frame + 1;
            Log(L"Netplay: players exist from frame %u", g_playStartFrame);
        }
    }
    if (g_syncEvery > 0 && g_frame % g_syncEvery == 0) {
        unsigned h = HashPlayers();
        int k = (g_frame / g_syncEvery) % 8;
        g_myHash[k] = h;
        g_myHashFrame[k] = g_frame;
        PktHash p{{'H', (BYTE)g_slot}, g_frame, h};
        SendAll(&p, sizeof(p));
        for (int s = 0; s < g_players; ++s)
            if (s != g_slot && g_peerHashFrame[s][k] == g_frame) CompareHash(s, g_frame, g_peerHash[s][k]);
        if (g_frame % (g_syncEvery * 10) == 0) LogPlayers(L"check");
    }

    ++g_frame;
    g_callsThisFrame = 0;
    unsigned future = g_frame + g_delay - 1;
    int r = future % kRing;
    g_inputs[r][g_slot] = SampleLocal(future);
    g_have[r][g_slot] = true;
    g_haveFrame[r][g_slot] = future;
    SendInputs(future);

    ULONGLONG until = GetTickCount64() + 10000;
    ULONGLONG nextResend = GetTickCount64() + 5;
    while (!HaveAll(g_frame)) {
        Receive();
        if (GetTickCount64() >= nextResend) { SendInputs(future); nextResend = GetTickCount64() + 5; }
        if (GetTickCount64() > until) {
            Log(L"Netplay: no input from a peer for 10 s at frame %u - continuing without it", g_frame);
            int q = g_frame % kRing;
            for (int s = 0; s < g_players; ++s)
                if (!g_have[q][s] || g_haveFrame[q][s] != g_frame) {
                    g_inputs[q][s] = Pad{}; g_have[q][s] = true; g_haveFrame[q][s] = g_frame;
                }
            break;
        }
        Sleep(0);
    }
    for (int s = 0; s < g_players; ++s) g_current[s] = g_inputs[g_frame % kRing][s];
    WaitForQuiescence();

    if (g_frameWaitMs > 0) {
        ULONGLONG waitUntil = GetTickCount64() + g_frameWaitMs;
        while (GetTickCount64() < waitUntil) Sleep(1);
    }
    if (g_frame % 600 == 0) {
        static ULONGLONG last = GetTickCount64();
        ULONGLONG now = GetTickCount64();
        Log(L"Netplay: frame %u (%.1f fps over the last 600 frames), pad syncs %ld (timeouts %ld)", g_frame,
            600000.0 / (now - last + 1), g_padSyncs, g_padSyncTimeouts);
        last = now;
        g_padSyncs = g_padSyncTimeouts = 0;
    }
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
    g_frameWaitMs = SettingInt(L"Netplay", L"_", L"FGUYMOD_NET_FRAMEWAIT", 0);
    g_idleScript = SettingInt(L"Netplay", L"_", L"FGUYMOD_NET_IDLE", 0) != 0;
    g_syncEvery = SettingInt(L"Netplay", L"SyncCheckFrames", nullptr, 60);
    for (int s = 0; s < 4; ++s)
        for (int k = 0; k < 8; ++k) g_peerHashFrame[s][k] = ~0u;
    for (int k = 0; k < 8; ++k) g_myHashFrame[k] = ~0u;
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
    };
    InstallHooks(exe, hooks);
}
