// Virtual controller: lets tools (and later, the network layer) press buttons on an Xbox pad.
//
// The game polls XInputGetState (xinput1_3 ordinal 2). We merge queued virtual button presses into
// the result, and report the pad as connected while virtual input is in use.
//
// Test tools talk to us over a named pipe, \\.\pipe\FGUYMod_<pid>, one command per line:
//   press <pad 0-3> <button> [<button> ...]    buttons: a b x y start back up down left right lb rb
//   quit                                        exit the game cleanly (test tools)
// Each button is held for kHoldMs, then released for kGapMs before the next one.
// Off unless FGUYMod.ini [Debug] TestInput=1 (or env FGUYMOD_TESTINPUT=1).
//
// The hook is always on, though, to note when each controller button was last pressed: the menus get
// some buttons as letter keys (A, LB = L, RB = R, X, Y), and the online lobby asks VPadPressedWithin to
// tell those from letters typed on a keyboard (lobby codes).

#include "common.h"

#include <xinput.h>

#include <cstdio>
#include <cwchar>
#include <deque>
#include <mutex>

namespace {

const ULONGLONG kHoldMs = 40, kGapMs = 260;  // short taps: the menus auto-repeat held buttons

struct Press {
    DWORD pad;
    WORD buttons;
};

std::mutex g_lock;
std::deque<Press> g_queue;
ULONGLONG g_pressStart = 0;      // when the front press started
bool g_padActive[4] = {};        // pads we report as connected
DWORD g_packet = 1;

using XInputGetState_t = DWORD(WINAPI*)(DWORD, XINPUT_STATE*);
XInputGetState_t Real_XInputGetState;

// When each button (bit) last went down, on any pad.
volatile ULONGLONG g_downAt[16];
WORD g_lastSeen[4];

void NoteButtons(DWORD pad, WORD buttons) {
    if (pad >= 4) return;
    WORD down = buttons & ~g_lastSeen[pad];
    g_lastSeen[pad] = buttons;
    if (!down) return;
    ULONGLONG now = GetTickCount64();
    for (int b = 0; b < 16; ++b)
        if (down & (1 << b)) g_downAt[b] = now;
}

// Buttons currently held on `pad` by the queue (advances the queue as time passes).
WORD CurrentButtons(DWORD pad) {
    std::lock_guard<std::mutex> lock(g_lock);
    ULONGLONG now = GetTickCount64();
    while (!g_queue.empty()) {
        if (g_pressStart == 0) g_pressStart = now;
        ULONGLONG t = now - g_pressStart;
        if (t < kHoldMs) return g_queue.front().pad == pad ? g_queue.front().buttons : 0;
        if (t < kHoldMs + kGapMs) return 0;
        g_queue.pop_front();
        g_pressStart = 0;
    }
    return 0;
}

}  // namespace
bool NetplayPad(DWORD pad, XINPUT_STATE* state, DWORD(WINAPI* real)(DWORD, XINPUT_STATE*));
WORD VPadButtons(DWORD pad) { return CurrentButtons(pad); }

// Is a real controller plugged in (pads 0-3)?
bool VPadRealPadConnected() {
    if (!Real_XInputGetState) return false;
    for (DWORD pad = 0; pad < 4; ++pad) {
        XINPUT_STATE st;
        if (Real_XInputGetState(pad, &st) == ERROR_SUCCESS) return true;
    }
    return false;
}

// Did any of these buttons go down on a controller in the last `ms` milliseconds?
bool VPadPressedWithin(WORD buttons, DWORD ms) {
    ULONGLONG now = GetTickCount64();
    for (int b = 0; b < 16; ++b)
        if ((buttons & (1 << b)) && g_downAt[b] && now - g_downAt[b] <= ms) return true;
    return false;
}

// A press after a delay (the session autopilot's presses in test copies without netplay).
struct DelayedPress { DWORD pad; WORD buttons; DWORD ms; };
DWORD WINAPI DelayedPressThread(LPVOID arg) {
    auto d = static_cast<DelayedPress*>(arg);
    Sleep(d->ms);
    {
        std::lock_guard<std::mutex> lock(g_lock);
        g_padActive[d->pad & 3] = true;
        g_queue.push_back({d->pad & 3, d->buttons});
    }
    delete d;
    return 0;
}
void VPadPressLater(DWORD pad, WORD buttons, DWORD ms) {
    CloseHandle(CreateThread(nullptr, 0, DelayedPressThread, new DelayedPress{pad, buttons, ms}, 0, nullptr));
}
namespace {

DWORD WINAPI Hook_XInputGetState(DWORD pad, XINPUT_STATE* state) {
    if (state && NetplayPad(pad, state, Real_XInputGetState))
        return NetplayPadConnected(pad) ? ERROR_SUCCESS : ERROR_DEVICE_NOT_CONNECTED;
    DWORD r = Real_XInputGetState(pad, state);
    if (IsBot() && state && r == ERROR_SUCCESS) ZeroMemory(&state->Gamepad, sizeof(state->Gamepad));  // not your pad
    static LONG calls[4];
    if (pad < 4 && InterlockedIncrement(&calls[pad]) == 1)
        Log(L"VPad: game polls pad %lu (real pad %s)", pad, r == ERROR_SUCCESS ? L"connected" : L"absent");
    if (pad >= 4 || !g_padActive[pad] || !state) {
        if (state) NoteButtons(pad, r == ERROR_SUCCESS ? state->Gamepad.wButtons : 0);
        return r;
    }
    if (r != ERROR_SUCCESS) ZeroMemory(state, sizeof(*state));  // no real pad: pretend one is there
    WORD buttons = CurrentButtons(pad);
    static WORD last[4];
    if (buttons != last[pad]) {
        last[pad] = buttons;
        ++g_packet;
        Log(L"VPad: pad %lu buttons -> %04x (poll #%ld)", pad, buttons, calls[pad]);
    }
    state->Gamepad.wButtons |= buttons;
    state->dwPacketNumber += g_packet;
    NoteButtons(pad, state->Gamepad.wButtons);
    return ERROR_SUCCESS;
}

WORD ButtonFromName(const char* n) {
    static const struct { const char* name; WORD bit; } table[] = {
        {"a", XINPUT_GAMEPAD_A}, {"b", XINPUT_GAMEPAD_B}, {"x", XINPUT_GAMEPAD_X},
        {"y", XINPUT_GAMEPAD_Y}, {"start", XINPUT_GAMEPAD_START}, {"back", XINPUT_GAMEPAD_BACK},
        {"up", XINPUT_GAMEPAD_DPAD_UP}, {"down", XINPUT_GAMEPAD_DPAD_DOWN},
        {"left", XINPUT_GAMEPAD_DPAD_LEFT}, {"right", XINPUT_GAMEPAD_DPAD_RIGHT},
        {"lb", XINPUT_GAMEPAD_LEFT_SHOULDER}, {"rb", XINPUT_GAMEPAD_RIGHT_SHOULDER},
    };
    for (auto& e : table)
        if (_stricmp(n, e.name) == 0) return e.bit;
    return 0;
}

void HandleCommand(char* line) {
    char* ctx = nullptr;
    char* cmd = strtok_s(line, " \t\r\n", &ctx);
    if (cmd && _stricmp(cmd, "quit") == 0) {  // test tools: exit cleanly (see VideoRequestQuit)
        VideoRequestQuit();
        // Paused games don't present frames, so that check may never come: then exit from here (still a
        // normal exit - only killing the process upsets the graphics driver).
        CloseHandle(CreateThread(nullptr, 0, [](LPVOID) -> DWORD {
            Sleep(3000);
            Log(L"VPad: no frame since the quit request - exiting");
            ExitProcess(0);
        }, nullptr, 0, nullptr));
        return;
    }
    if (cmd && _stricmp(cmd, "kbm") == 0) {  // test keyboard / mouse (online keyboard players, kbm.cpp)
        char* what = strtok_s(nullptr, " \t\r\n", &ctx);
        char* a = strtok_s(nullptr, " \t\r\n", &ctx);
        char* b = strtok_s(nullptr, " \t\r\n", &ctx);
        char* c = strtok_s(nullptr, " \t\r\n", &ctx);
        if (what) KbmTestCommand(what, a ? atoi(a) : 0, b ? atoi(b) : 0, c ? atoi(c) : 0);
        return;
    }
    if (!cmd || _stricmp(cmd, "press") != 0) return;
    char* padStr = strtok_s(nullptr, " \t\r\n", &ctx);
    if (!padStr) return;
    DWORD pad = (DWORD)atoi(padStr) & 3;
    std::lock_guard<std::mutex> lock(g_lock);
    g_padActive[pad] = true;
    while (char* b = strtok_s(nullptr, " \t\r\n", &ctx)) {
        if (WORD bit = ButtonFromName(b)) g_queue.push_back({pad, bit});
        else Log(L"VPad: unknown button \"%S\"", b);
    }
}

DWORD WINAPI PipeThread(LPVOID) {
    wchar_t name[64];
    swprintf_s(name, L"\\\\.\\pipe\\FGUYMod_%lu", GetCurrentProcessId());
    for (;;) {
        HANDLE pipe = CreateNamedPipeW(name, PIPE_ACCESS_INBOUND, PIPE_TYPE_BYTE | PIPE_WAIT, 1, 0,
                                       4096, 0, nullptr);
        if (pipe == INVALID_HANDLE_VALUE) {
            Log(L"VPad: could not create pipe %s", name);
            return 1;
        }
        if (ConnectNamedPipe(pipe, nullptr) || GetLastError() == ERROR_PIPE_CONNECTED) {
            char buf[4096];
            size_t used = 0;
            DWORD got;
            while (ReadFile(pipe, buf + used, (DWORD)(sizeof(buf) - 1 - used), &got, nullptr) && got) {
                used += got;
                buf[used] = 0;
                char* start = buf;
                while (char* nl = strchr(start, '\n')) {
                    *nl = 0;
                    Log(L"VPad: %S", start);
                    HandleCommand(start);
                    start = nl + 1;
                }
                used = strlen(start);
                memmove(buf, start, used + 1);
            }
            if (used) {
                Log(L"VPad: %S", buf);
                HandleCommand(buf);
            }
        }
        DisconnectNamedPipe(pipe);
        CloseHandle(pipe);
    }
}

}  // namespace

void InitVirtualPad(HMODULE exe) {
    bool testInput = SettingInt(L"Debug", L"TestInput", L"FGUYMOD_TESTINPUT", 0) != 0;
    HookSpec hooks[] = {
        {"xinput1_3.dll", MAKEINTRESOURCEA(2), (void*)Hook_XInputGetState, (void**)&Real_XInputGetState},
    };
    InstallHooks(exe, hooks);
    if (!testInput) return;
    if (IsBot()) {   // "Stop test bot.bat" closes every bot copy through its pipe
        wchar_t path[MAX_PATH];
        swprintf_s(path, L"%s\\bot_pids.txt", g_modDir);
        HANDLE f = CreateFileW(path, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS, 0, nullptr);
        char line[32];
        int n = sprintf_s(line, "%lu\r\n", GetCurrentProcessId());
        DWORD put;
        if (f != INVALID_HANDLE_VALUE) { WriteFile(f, line, n, &put, nullptr); CloseHandle(f); }
    }
    g_padActive[0] = true;  // plugged in from the start, so the game has noticed it before any press
    CloseHandle(CreateThread(nullptr, 0, PipeThread, nullptr, 0, nullptr));
    Log(L"VPad: test input pipe \\\\.\\pipe\\FGUYMod_%lu ready", GetCurrentProcessId());
}
