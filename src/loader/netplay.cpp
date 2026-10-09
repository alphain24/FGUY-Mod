// Netplay core: lockstep input sync.
//
// Every peer runs the same local multiplayer match. Each peer owns one player slot (= one virtual
// XInput pad). Every frame, each peer sends its pad state for (frame + InputDelay) to the others and
// waits until it has every slot's state for the current frame, so all peers feed identical inputs
// into identical frames. For that to stay in sync the game must be deterministic, so while netplay
// runs we also:
//   * give the game's main thread a fixed clock: exactly 1/60 s per frame (QPC, timeGetTime, GetTickCount)
//   * seed the C runtime random generator with a fixed value
// and every SyncCheckFrames frames peers compare a hash of the players' positions (desync detector).
//
// Start barrier: a peer becomes "ready" when its match is loaded (players exist, then ReadyDelay
// seconds, for now). It then holds at the end of that frame until every peer is ready, and frame 0
// begins everywhere at once.
//
// Settings ([Netplay] in FGUYMod.ini, env overrides for tests):
//   Enable=1  Slot=0..3  Players=2  Port=7600  Peers=127.0.0.1:7601,...  InputDelay=3
//   Script=1 (scripted test inputs instead of the real pad)  ReadyDelay=5  SyncCheckFrames=60

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
int g_slot = 0, g_players = 2, g_delay = 3, g_syncEvery = 60, g_readyDelay = 5;
bool g_script = false;
bool g_traceEnabled = false;
int g_frameWaitMs = 0;
bool g_idleAfterResume = false;
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
// The game reads gameplay input on its own pad thread (FGUY.exe 0x47b080 -> 0x4cc4a0), which the game
// runs once per frame, alongside the frame. Left alone, a pass could run before we switch to the next
// frame's inputs on one PC and after on another. So each pass waits at its first poll (pad 1) until the
// render thread has switched to the next frame's inputs (g_frameReady): every pass then reads the same
// frame's inputs on every PC. The pass must also be finished (stored) before the next frame starts:
// a pass polls pads 1-4 in order (measured), so once pad 4 has been polled with the new inputs, the
// render thread allows 1 ms for the last result to be stored and then starts the next frame.
HANDLE g_frameReady = nullptr;
volatile LONG g_passesBegun = 0;     // pad thread was let through the gate
volatile LONG g_lastPadPolls = 0;    // polls of pad 4 by the pad thread

// Research: a timeline of pad-related events for a few frames (env FGUYMOD_NET_TRACE=1).
struct TraceEvent { LONGLONG t; DWORD thread; int what; int arg; };
TraceEvent g_trace[4096];
volatile LONG g_traceCount = 0;
bool g_traceOn = false;
void Trace(int what, int arg) {
    if (!g_traceOn) return;
    LONG i = InterlockedIncrement(&g_traceCount) - 1;
    if (i >= 4096) return;
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);  // our own import: real time
    g_trace[i] = {t.QuadPart, GetCurrentThreadId(), what, arg};
}


enum class Phase { Off, WaitingLocal, Barrier, Running };
Phase g_phase = Phase::Off;
unsigned g_frame = 0;                // lockstep frame number (frame being simulated next)
ULONGLONG g_localReadyAt = 0;
bool g_peerReady[4];
DWORD g_mainThread = 0;
SOCKET g_sock = INVALID_SOCKET;

// ---- fixed clock --------------------------------------------------------------------------------------
LARGE_INTEGER g_freq, g_qpcBase;
DWORD g_msBase = 0;
LONGLONG g_callsThisFrame = 0;
decltype(&QueryPerformanceCounter) Real_QPC;
decltype(&GetTickCount) Real_GetTickCount;
using timeGetTime_t = DWORD(WINAPI*)();
timeGetTime_t Real_timeGetTime;
using srand_t = void(__cdecl*)(unsigned);
srand_t Real_srand;

bool FixedClock() { return g_phase == Phase::Running && GetCurrentThreadId() == g_mainThread; }

BOOL WINAPI Hook_QPC(LARGE_INTEGER* out) {
    if (!FixedClock()) return Real_QPC(out);
    // 1/60 s per frame, plus one tick per call so "wait until time passes" loops still finish.
    out->QuadPart = g_qpcBase.QuadPart + (LONGLONG)g_frame * g_freq.QuadPart / 60 + (++g_callsThisFrame);
    return TRUE;
}
DWORD WINAPI Hook_GetTickCount() {
    return FixedClock() ? g_msBase + (DWORD)((ULONGLONG)g_frame * 1000 / 60) : Real_GetTickCount();
}
DWORD WINAPI Hook_timeGetTime() {
    return FixedClock() ? g_msBase + (DWORD)((ULONGLONG)g_frame * 1000 / 60) : Real_timeGetTime();
}
void __cdecl Hook_srand(unsigned) { Real_srand(0x46475559); }  // "FGUY"

// The menus react to mouse movement; during netplay the local mouse must not change anything.
decltype(&GetCursorPos) Real_GetCursorPos;
BOOL WINAPI Hook_GetCursorPos(POINT* pt) {
    if (!pt) return FALSE;
    pt->x = 0;
    pt->y = 0;
    return TRUE;
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
unsigned g_peerHashFrame[4][8] = {{~0u, ~0u, ~0u, ~0u, ~0u, ~0u, ~0u, ~0u}, {~0u, ~0u, ~0u, ~0u, ~0u, ~0u, ~0u, ~0u},
                                  {~0u, ~0u, ~0u, ~0u, ~0u, ~0u, ~0u, ~0u}, {~0u, ~0u, ~0u, ~0u, ~0u, ~0u, ~0u, ~0u}};
unsigned g_myHash[8], g_myHashFrame[8] = {~0u, ~0u, ~0u, ~0u, ~0u, ~0u, ~0u, ~0u};
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
                int r = f % kRing;
                if (f + kRing <= g_frame + kRing / 2) continue;  // far too old
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

Pad ScriptPad(unsigned f) {
    // A fixed little routine: (slot 0) press A to leave the pause menu the level starts in, (slot 1)
    // press Start to drop in, then walk around and jump/attack.
    Pad p{};
    unsigned t = f % 1200;
    if (g_slot == 0 && f >= 20 && f < 23) p.buttons |= XINPUT_GAMEPAD_A;
    if (g_slot == 1 && f >= 60 && f < 63) p.buttons |= XINPUT_GAMEPAD_START;
    if (t >= 120 && t < 360) p.ly = 30000;
    if (t >= 360 && t < 480) p.lx = 30000;
    if (t >= 480 && t < 720) p.ly = -30000;
    if (t >= 720 && t < 840) p.lx = -30000;
    if (t % 97 < 3 && f > 120) p.buttons |= XINPUT_GAMEPAD_A;
    if (t % 53 < 3 && f > 120) p.buttons |= XINPUT_GAMEPAD_X;
    return p;
}

Pad SampleLocal(unsigned f) {
    if (g_script && g_idleAfterResume) {  // research: resume the pause menu, then do nothing
        Pad p{};
        if (g_slot == 0 && f >= 20 && f < 23) p.buttons |= XINPUT_GAMEPAD_A;
        return p;
    }
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
void StartRunning() {
    g_phase = Phase::Running;
    g_frame = 0;
    Real_QPC(&g_qpcBase);
    g_msBase = Real_GetTickCount();
    ZeroMemory(g_have, sizeof(g_have));
    for (unsigned f = 0; f < (unsigned)g_delay; ++f)          // the first frames: everyone idle
        for (int s = 0; s < g_players; ++s) {
            g_inputs[f % kRing][s] = Pad{};
            g_have[f % kRing][s] = true;
            g_haveFrame[f % kRing][s] = f;
        }
    Log(L"Netplay: all %d players ready - lockstep started (slot %d, input delay %d frames)", g_players,
        g_slot, g_delay);
    LogPlayers(L"start");
}

}  // namespace

// Called by vpad.cpp's XInputGetState hook. Returns true if netplay supplies this pad's state.
bool NetplayPad(DWORD pad, XINPUT_STATE* state, DWORD(WINAPI* real)(DWORD, XINPUT_STATE*)) {
    if (!g_enabled) return false;
    g_realXInput = real;
    Trace(1, (int)pad);  // poll
    if (g_phase == Phase::Running && g_mainThread && GetCurrentThreadId() != g_mainThread) {
        if (pad == 0) {
            Trace(2, 0);  // gate wait
            WaitForSingleObject(g_frameReady, 250);  // run this pass with the next frame's inputs
            InterlockedIncrement(&g_passesBegun);
            Trace(3, 0);  // gate passed
        } else if (pad == 3) {
            InterlockedIncrement(&g_lastPadPolls);
        }
    }
    if (g_phase != Phase::Running) {
        // Before the match: no pads at all, so nothing the player does locally changes the game.
        if (pad < 4 && (int)pad < g_players) { ZeroMemory(state, sizeof(*state)); state->dwPacketNumber = 1; return true; }
        return pad < 4;
    }
    if (pad >= 4 || (int)pad >= g_players) return true;  // unused slots: report "not connected"
    static DWORD packet[4];
    static Pad last[4];
    Pad p = g_current[pad];
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
    if (!g_mainThread) g_mainThread = GetCurrentThreadId();
    g_callsThisFrame = 0;
    Receive();

    if (g_phase == Phase::WaitingLocal) {
        void* players[8];
        if (ResearchPlayers(players, 8) > 0) {
            if (!g_localReadyAt) {
                g_localReadyAt = GetTickCount64() + g_readyDelay * 1000ULL;
                Log(L"Netplay: match loaded, ready in %d s", g_readyDelay);
            } else if (GetTickCount64() >= g_localReadyAt) {
                g_phase = Phase::Barrier;
                g_peerReady[g_slot] = true;
                Log(L"Netplay: ready, waiting for the other players");
            }
        }
        return;
    }
    if (g_phase == Phase::Barrier) {
        // Hold this frame until everyone is ready (the game is frozen meanwhile).
        PktHeader r{'R', (BYTE)g_slot};
        ULONGLONG until = GetTickCount64() + 120000;
        for (;;) {
            SendAll(&r, sizeof(r));
            Receive();
            bool all = true;
            for (int s = 0; s < g_players; ++s) all &= g_peerReady[s];
            if (all || GetTickCount64() > until) break;
            Sleep(20);
        }
        for (int i = 0; i < 10; ++i) { SendAll(&r, sizeof(r)); Sleep(5); }  // let late peers see us
        StartRunning();
    }

    // Running: frame g_frame has just been simulated and shown.
    if (g_syncEvery > 0 && g_frame % g_syncEvery == 0) {
        unsigned h = HashPlayers();
        int k = (g_frame / g_syncEvery) % 8;
        g_myHash[k] = h;
        g_myHashFrame[k] = g_frame;
        PktHash p{{'H', (BYTE)g_slot}, g_frame, h};
        SendAll(&p, sizeof(p));
        for (int s = 0; s < g_players; ++s)
            if (s != g_slot && g_peerHashFrame[s][k] == g_frame) CompareHash(s, g_frame, g_peerHash[s][k]);
        if (g_frame % (g_syncEvery * 10) == 0 || g_desyncs == 0) LogPlayers(L"check");
    }

    ++g_frame;
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
            for (int s = 0; s < g_players; ++s) {
                int q = g_frame % kRing;
                if (!g_have[q][s] || g_haveFrame[q][s] != g_frame) {
                    g_inputs[q][s] = Pad{}; g_have[q][s] = true; g_haveFrame[q][s] = g_frame;
                }
            }
            break;
        }
        Sleep(0);
    }
    for (int s = 0; s < g_players; ++s) g_current[s] = g_inputs[g_frame % kRing][s];
    g_traceOn = g_traceEnabled && g_frame >= 300 && g_frame < 306;
    Trace(4, (int)g_frame);  // release
    SetEvent(g_frameReady);
    // Research knob: extra real time per frame (FGUYMOD_NET_FRAMEWAIT, ms).
    if (g_frameWaitMs > 0) {
        ULONGLONG waitUntil = GetTickCount64() + g_frameWaitMs;
        while (GetTickCount64() < waitUntil) Sleep(1);
    }
    Trace(5, (int)g_frame);  // next frame starts
    if (g_traceEnabled && g_frame == 306) {
        LONGLONG t0 = g_trace[0].t;
        const wchar_t* names[] = {L"?", L"poll pad", L"gate wait", L"gate passed", L"RELEASE frame", L"FRAME START"};
        for (LONG i = 0; i < g_traceCount && i < 4096; ++i)
            Log(L"Trace %8.3f ms  thread %5lu  %s %d", (g_trace[i].t - t0) * 1000.0 / g_freq.QuadPart, g_trace[i].thread,
                names[g_trace[i].what], g_trace[i].arg);
    }
    if (false) {
        static int warned = 0;
        if (warned++ < 5) Log(L"Netplay: the game's pad thread was slow to pick up frame %u's input", g_frame);
    }

    if (g_frame % 600 == 0) {
        static ULONGLONG last = GetTickCount64();
        ULONGLONG now = GetTickCount64();
        Log(L"Netplay: frame %u (%.1f fps over the last 600 frames)", g_frame, 600000.0 / (now - last + 1));
        last = now;
    }
}

bool NetplayEnabled() { return g_enabled; }

void InitNetplay(HMODULE exe) {
    g_enabled = SettingInt(L"Netplay", L"Enable", L"FGUYMOD_NET", 0) != 0;
    if (!g_enabled) return;
    g_slot = SettingInt(L"Netplay", L"Slot", L"FGUYMOD_NET_SLOT", 0) & 3;
    g_players = SettingInt(L"Netplay", L"Players", L"FGUYMOD_NET_PLAYERS", 2);
    g_delay = SettingInt(L"Netplay", L"InputDelay", L"FGUYMOD_NET_DELAY", 3);
    g_script = SettingInt(L"Netplay", L"Script", L"FGUYMOD_NET_SCRIPT", 0) != 0;
    g_traceEnabled = SettingInt(L"Netplay", L"_", L"FGUYMOD_NET_TRACE", 0) != 0;
    g_frameWaitMs = SettingInt(L"Netplay", L"_", L"FGUYMOD_NET_FRAMEWAIT", 0);
    g_idleAfterResume = SettingInt(L"Netplay", L"_", L"FGUYMOD_NET_IDLE", 0) != 0;
    g_readyDelay = SettingInt(L"Netplay", L"ReadyDelay", L"FGUYMOD_NET_READYDELAY", 5);
    g_syncEvery = SettingInt(L"Netplay", L"SyncCheckFrames", nullptr, 60);
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
    g_frameReady = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    g_phase = Phase::WaitingLocal;
    Log(L"Netplay: on - slot %d of %d players, UDP port %d, %zu peers, input delay %d, %s input",
        g_slot, g_players, port, g_peers.size(), g_delay, g_script ? L"scripted" : L"controller");

    HookSpec hooks[] = {
        {"kernel32.dll", "QueryPerformanceCounter", (void*)Hook_QPC, (void**)&Real_QPC},
        {"kernel32.dll", "GetTickCount", (void*)Hook_GetTickCount, (void**)&Real_GetTickCount},
        {"winmm.dll", "timeGetTime", (void*)Hook_timeGetTime, (void**)&Real_timeGetTime},
        {"msvcr100.dll", "srand", (void*)Hook_srand, (void**)&Real_srand},
        {"user32.dll", "GetCursorPos", (void*)Hook_GetCursorPos, (void**)&Real_GetCursorPos},
    };
    InstallHooks(exe, hooks);
}
