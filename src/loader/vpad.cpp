// Virtual controller: lets tools (and later, the network layer) press buttons on an Xbox pad.
//
// The game polls XInputGetState (xinput1_3 ordinal 2). We merge queued virtual button presses into
// the result, and report the pad as connected while virtual input is in use.
//
// Test tools talk to us over a named pipe, \\.\pipe\FGUYMod_<pid>, one command per line:
//   press <pad 0-3> <button> [<button> ...]    buttons: a b x y start back up down left right lb rb
//   quit                                        exit the game cleanly (test tools)
//   hold <pad> <ms> [lx= ly= rx= ry= lt= rt=] [<button> ...]   sticks (-32768..32767), triggers (0..255) and
//                                               buttons held for <ms> (research: moving, aiming, firing)
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
struct Hold {
    ULONGLONG until;
    SHORT lx, ly, rx, ry;
    BYTE lt, rt;
    WORD buttons;
} g_hold[4];
ULONGLONG g_pressStart = 0;      // when the front press started
bool g_padActive[4] = {};        // pads we report as connected
DWORD g_packet = 1;

using XInputGetState_t = DWORD(WINAPI*)(DWORD, XINPUT_STATE*);
XInputGetState_t Real_XInputGetState;
using XInputSetState_t = DWORD(WINAPI*)(DWORD, XINPUT_VIBRATION*);
XInputSetState_t Real_XInputSetState;

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

// The test pipe's "hold" (sticks, triggers, buttons) for `pad`, while it lasts (netplay.cpp: my input in a session).
bool VPadHeld(DWORD pad, XINPUT_GAMEPAD* g) {
    if (pad >= 4) return false;
    std::lock_guard<std::mutex> lock(g_lock);
    const Hold& h = g_hold[pad];
    if (h.until <= GetTickCount64()) return false;
    g->wButtons = h.buttons;
    g->sThumbLX = h.lx; g->sThumbLY = h.ly;
    g->sThumbRX = h.rx; g->sThumbRY = h.ry;
    g->bLeftTrigger = h.lt; g->bRightTrigger = h.rt;
    return true;
}

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

// the real pad, the emote buttons taken out (emotes.cpp) - what the game, or the lockstep, reads
DWORD WINAPI PadRead(DWORD pad, XINPUT_STATE* state) {
    DWORD r = Real_XInputGetState(pad, state);
    if (r == ERROR_SUCCESS) EmotesPad(pad, state);
    return r;
}

DWORD WINAPI Hook_XInputGetState(DWORD pad, XINPUT_STATE* state) {
    if (pad == 0) AudioTick();
    if (state && NetplayPad(pad, state, PadRead)) {
        if (pad == 0) ModsTick();   // the mods' plugins (after the lockstep has this tick's inputs)
        return NetplayPadConnected(pad) ? ERROR_SUCCESS : ERROR_DEVICE_NOT_CONNECTED;
    }
    if (pad == 0) ModsTick();
    DWORD r = Real_XInputGetState(pad, state);   // (offline the emote wheel looks at it last: the test pipe's presses too)
    if (IsBot() && state && r == ERROR_SUCCESS) ZeroMemory(&state->Gamepad, sizeof(state->Gamepad));  // not your pad
    static LONG calls[4];
    if (pad < 4 && InterlockedIncrement(&calls[pad]) == 1)
        Log(L"VPad: game polls pad %lu (real pad %s)", pad, r == ERROR_SUCCESS ? L"connected" : L"absent");
    if (pad >= 4 || !g_padActive[pad] || !state) {
        if (state && r == ERROR_SUCCESS) EmotesPad(pad, state);
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
    {
        std::lock_guard<std::mutex> lock(g_lock);
        const Hold& h = g_hold[pad];
        if (h.until > GetTickCount64()) {
            state->Gamepad.wButtons |= h.buttons;
            state->Gamepad.sThumbLX = h.lx; state->Gamepad.sThumbLY = h.ly;
            state->Gamepad.sThumbRX = h.rx; state->Gamepad.sThumbRY = h.ry;
            state->Gamepad.bLeftTrigger = h.lt; state->Gamepad.bRightTrigger = h.rt;
            ++g_packet;
        }
    }
    state->dwPacketNumber += g_packet;
    EmotesPad(pad, state);
    NoteButtons(pad, state->Gamepad.wButtons);
    return ERROR_SUCCESS;
}

// Rumble. In an online match every player's controller is a pad of the game's (pad n = player slot n): only my
// slot's rumble is for my controller here (the first real pad), and only if I want it ([Online] Vibration).
DWORD WINAPI Hook_XInputSetState(DWORD pad, XINPUT_VIBRATION* v) {
    if (!NetplayEnabled()) return Real_XInputSetState(pad, v);
    if ((int)pad != NetplayLocalSlot() || StaysMuted()) return ERROR_SUCCESS;   // (the test bot: never yours)
    XINPUT_VIBRATION none = {};
    return Real_XInputSetState(0, OnlineVibration() && v ? v : &none);
}

WORD ButtonFromName(const char* n) {
    static const struct { const char* name; WORD bit; } table[] = {
        {"a", XINPUT_GAMEPAD_A}, {"b", XINPUT_GAMEPAD_B}, {"x", XINPUT_GAMEPAD_X},
        {"y", XINPUT_GAMEPAD_Y}, {"start", XINPUT_GAMEPAD_START}, {"back", XINPUT_GAMEPAD_BACK},
        {"up", XINPUT_GAMEPAD_DPAD_UP}, {"down", XINPUT_GAMEPAD_DPAD_DOWN},
        {"left", XINPUT_GAMEPAD_DPAD_LEFT}, {"right", XINPUT_GAMEPAD_DPAD_RIGHT},
        {"lb", XINPUT_GAMEPAD_LEFT_SHOULDER}, {"rb", XINPUT_GAMEPAD_RIGHT_SHOULDER},
        {"ls", XINPUT_GAMEPAD_LEFT_THUMB}, {"rs", XINPUT_GAMEPAD_RIGHT_THUMB},
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
        // A game whose window can't be seen (covered by other windows) or that's paused stops presenting
        // frames, and the quit happens at the next one: bring the window up (without the focus) so it draws.
        // If no frame comes even so, exit from here - the last resort (ending a game mid-frame can leave the
        // graphics driver refusing new devices for a few minutes).
        DisplayShowForQuit();
        CloseHandle(CreateThread(nullptr, 0, [](LPVOID) -> DWORD {
            Sleep(5000);
            Log(L"VPad: no frame since the quit request - exiting");
            QuitGame();
            return 0;
        }, nullptr, 0, nullptr));
        return;
    }
    if (cmd && _stricmp(cmd, "shot") == 0) {   // test tools: shot <file.bmp> - the next frame (d3d9.cpp)
        char* path = strtok_s(nullptr, "\r\n", &ctx);
        while (path && *path == ' ') ++path;
        if (path && *path) {
            wchar_t w[1024];
            if (MultiByteToWideChar(CP_UTF8, 0, path, -1, w, 1024)) VideoRequestShot(w);
            else Log(L"VPad: shot - the path is too long");
        }
        return;
    }
    if (cmd && _stricmp(cmd, "netstall") == 0) {   // test tools: netstall <ms> - this PC goes quiet for a while
        char* ms = strtok_s(nullptr, " \t\r\n", &ctx);
        NetplayTestStall(ms ? (DWORD)atoi(ms) : 5000);
        return;
    }
    if (cmd && _stricmp(cmd, "profile") == 0) {   // research: profile <ms> - where the render thread spends its time
        char* ms = strtok_s(nullptr, " \t\r\n", &ctx);
        static DWORD len;
        len = ms ? (DWORD)atoi(ms) : 3000;
        CloseHandle(CreateThread(nullptr, 0, [](LPVOID) -> DWORD { NetplayProfileRender(len); return 0; }, nullptr, 0, nullptr));
        return;
    }
    if (cmd && _stricmp(cmd, "netleave") == 0) {   // test tools: quit the match to its lobby (the pause menu's QUIT)
        NetplayTestLeave();
        return;
    }
    if (cmd && _stricmp(cmd, "crashexit") == 0) {   // test tools: end this game as a crash would (exit code 0xC0000005)
        Log(L"VPad: test - ending this game as a crash would");
        TerminateProcess(GetCurrentProcess(), 0xC0000005);
        return;
    }
    if (cmd && _stricmp(cmd, "resolution") == 0) {   // test tools: resolution <w> <h> - as Options > Screen does
        char* w = strtok_s(nullptr, " \t\r\n", &ctx);
        char* h = strtok_s(nullptr, " \t\r\n", &ctx);
        if (w && h) NetplayTestResolution(atoi(w), atoi(h));
        return;
    }
    if (cmd && _stricmp(cmd, "winkey") == 0) {   // test tools: winkey <vk> <ms> - a key held in the game's own key
        char* k = strtok_s(nullptr, " \t\r\n", &ctx);   // table (as if typed on the game window; menus, not netplay)
        char* ms = strtok_s(nullptr, " \t\r\n", &ctx);
        if (k) {
            static int vk;
            static DWORD hold;
            vk = atoi(k) & 0xff;
            hold = ms ? (DWORD)atoi(ms) : 500;
            CloseHandle(CreateThread(nullptr, 0, [](LPVOID) -> DWORD {
                auto wnd = *reinterpret_cast<unsigned char**>(0xad39e8);
                if (!wnd) return 0;
                Log(L"VPad: winkey %d held for %lu ms", vk, hold);
                ULONGLONG until = GetTickCount64() + hold;
                while (GetTickCount64() < until) { wnd[0xc2 + vk] = 0x80; Sleep(1); }
                wnd[0xc2 + vk] = 0;
                return 0;
            }, nullptr, 0, nullptr));
        }
        return;
    }
    if (cmd && _stricmp(cmd, "typewin") == 0) {   // test tools: typewin <vk> <text...> - a key down on the game window,
        char* k = strtok_s(nullptr, " \t\r\n", &ctx);   // then the text as typed characters, then Enter (the lobby's chat)
        HWND wnd = DisplayMainWindow();
        if (k && wnd) {
            int vk = atoi(k) & 0xff;
            if (vk) PostMessageW(wnd, WM_KEYDOWN, vk, 0);
            Sleep(300);
            std::string text = ctx ? ctx : "";
            while (!text.empty() && (text.back() == '\r' || text.back() == '\n')) text.pop_back();
            for (char ch : text) PostMessageW(wnd, WM_CHAR, (WPARAM)(unsigned char)ch, 0);
            PostMessageW(wnd, WM_CHAR, '\r', 0);
            Log(L"VPad: typewin %d '%S'", vk, text.c_str());
        }
        return;
    }
    if (cmd && _stricmp(cmd, "yaw") == 0) {   // test tools: the lobby's 3D preview facing, into the log
        OnlineMenuLogYaw();
        return;
    }
    if (cmd && _stricmp(cmd, "hurt") == 0) {   // test tools: hurt <player> <amount> - a hit nobody caused (free-running)
        char* who = strtok_s(nullptr, " \t\r\n", &ctx);
        char* amount = strtok_s(nullptr, " \t\r\n", &ctx);
        if (who && amount) NetplayTestHurt(atoi(who), (float)atof(amount));
        return;
    }
    if (cmd && _stricmp(cmd, "hooktest") == 0) {   // test tools: hooktest <hex address> <length> - the mods' Hook, tried
        char* a = strtok_s(nullptr, " \t\r\n", &ctx);   //   (hooktest 0: how many calls went through it)
        char* n = strtok_s(nullptr, " \t\r\n", &ctx);
        ModsTestHook(a ? (unsigned)strtoul(a, nullptr, 16) : 0, n ? atoi(n) : 0);
        return;
    }
    if (cmd && _stricmp(cmd, "god") == 0) {   // test tools: god 1 / 0 - my player takes no hits (free-running)
        char* on = strtok_s(nullptr, " \t\r\n", &ctx);
        NetplayTestGod(!on || atoi(on) != 0);
        return;
    }
    if (cmd && _stricmp(cmd, "sendevent") == 0) {   // research: sendevent <id low hex> <id high hex> <event hex> - that
        char* lo = strtok_s(nullptr, " \t\r\n", &ctx);   //   object (by its asset id) gets the event here (free-running)
        char* hi = strtok_s(nullptr, " \t\r\n", &ctx);
        char* ev = strtok_s(nullptr, " \t\r\n", &ctx);
        if (lo && hi && ev)
            NetplayTestSendEvent((unsigned)strtoul(lo, nullptr, 16), (unsigned)strtoul(hi, nullptr, 16), (unsigned)strtoul(ev, nullptr, 16));
        return;
    }
    if (cmd && _stricmp(cmd, "pointon") == 0) {   // test tools: pointon <id hex> - the host's game switches that control
        char* id = strtok_s(nullptr, " \t\r\n", &ctx);   // point on (Infiltration, free-running)
        if (id) NetplayTestPointOn((unsigned)strtoul(id, nullptr, 16));
        return;
    }
    if (cmd && _stricmp(cmd, "killnpcs") == 0) {   // test tools: the host's enemies all down (free-running, enemy sync)
        NetplayTestKillNpcs();
        return;
    }
    if (cmd && _stricmp(cmd, "skinload") == 0) {   // test tools: skinload <slot> [file.fgskin] - that player wears it
        char* slot = strtok_s(nullptr, " \t\r\n", &ctx);
        char* path = strtok_s(nullptr, "\r\n", &ctx);
        wchar_t wpath[MAX_PATH] = L"";
        if (path) MultiByteToWideChar(CP_UTF8, 0, path, -1, wpath, MAX_PATH);
        if (slot) CosmeticsLoadSkinFile(atoi(slot), wpath);
        return;
    }
    if (cmd && (_stricmp(cmd, "poserec") == 0 || _stricmp(cmd, "posefreeze") == 0 || _stricmp(cmd, "poseplay") == 0 ||
                _stricmp(cmd, "posestop") == 0 || _stricmp(cmd, "findptr") == 0)) {
        char* a = strtok_s(nullptr, " \t\r\n", &ctx);   // research: characters' poses (motion.cpp)
        char* b = strtok_s(nullptr, " \t\r\n", &ctx);
        MotionCommand(cmd, a, b);
        return;
    }
    if (cmd && _stricmp(cmd, "emotekey") == 0) {   // test tools: emotekey <0|1> [dx dy] - B held / let go, the mouse moved
        char* down = strtok_s(nullptr, " \t\r\n", &ctx);
        char* dx = strtok_s(nullptr, " \t\r\n", &ctx);
        char* dy = strtok_s(nullptr, " \t\r\n", &ctx);
        if (down) EmotesTestKey(atoi(down) != 0, dx ? atoi(dx) : 0, dy ? atoi(dy) : 0);
        return;
    }
    if (cmd && _stricmp(cmd, "emote") == 0) {   // test tools: emote <player slot> <n> - the nth emote of its character
        char* slot = strtok_s(nullptr, " \t\r\n", &ctx);
        char* nth = strtok_s(nullptr, " \t\r\n", &ctx);
        if (slot && nth) EmotesRequest(atoi(slot), atoi(nth) - 1);
        return;
    }
    if (cmd && _stricmp(cmd, "texdraws") == 0) {   // research: texdraws <lo> <hi> - a frame's draws with that texture
        char* lo = strtok_s(nullptr, " \t\r\n", &ctx);
        char* hi = strtok_s(nullptr, " \t\r\n", &ctx);
        if (lo && hi) CosmeticsTrace(strtoul(lo, nullptr, 16), strtoul(hi, nullptr, 16));
        return;
    }
    if (cmd && (_stricmp(cmd, "texinfo") == 0 || _stricmp(cmd, "texfill") == 0)) {   // research: texinfo <lo> <hi> /
        char* lo = strtok_s(nullptr, " \t\r\n", &ctx);                                // texfill <lo> <hi> <rrggbb>
        char* hi = strtok_s(nullptr, " \t\r\n", &ctx);
        char* rgb = strtok_s(nullptr, " \t\r\n", &ctx);
        if (lo && hi) CosmeticsTest(_stricmp(cmd, "texfill") == 0, strtoul(lo, nullptr, 16), strtoul(hi, nullptr, 16),
                                    rgb ? strtoul(rgb, nullptr, 16) : 0xff00ff);
        return;
    }
    if (cmd && _stricmp(cmd, "uicmd") == 0) {   // test tools: uicmd <command> [args] - a menu command, as a menu sends it
        char* c = strtok_s(nullptr, " \t\r\n", &ctx);
        char* a = strtok_s(nullptr, "\r\n", &ctx);
        if (c) NetplayTestCommand(c, a ? a : "");
        return;
    }
    if (cmd && _stricmp(cmd, "freeze") == 0) {   // test tools: freeze <ms> - the game loop stops a while (a window held)
        char* ms = strtok_s(nullptr, " \t\r\n", &ctx);
        if (ms) NetplayTestFreeze(atoi(ms));
        return;
    }
    if (cmd && _stricmp(cmd, "hitnpc") == 0) {   // test tools: hitnpc <id hex> <amount> - my player hits that enemy
        char* id = strtok_s(nullptr, " \t\r\n", &ctx);
        char* amount = strtok_s(nullptr, " \t\r\n", &ctx);
        if (id && amount) NetplayTestHitNpc((unsigned)strtoul(id, nullptr, 16), atoi(amount));
        return;
    }
    if (cmd && _stricmp(cmd, "nudge") == 0) {   // test tools: this PC's game goes out of sync with the others
        NetplayTestNudge();
        return;
    }
    if (cmd && _stricmp(cmd, "peek") == 0) {   // research: peek <hex address> [dwords] [pointers to follow first]
        char* a = strtok_s(nullptr, " \t\r\n", &ctx);
        char* n = strtok_s(nullptr, " \t\r\n", &ctx);
        char* deref = strtok_s(nullptr, " \t\r\n", &ctx);
        DWORD at = a ? strtoul(a, nullptr, 16) : 0;
        for (int d = deref ? atoi(deref) : 0; d > 0 && !IsBadReadPtr((void*)at, 4); --d) {
            Log(L"Peek: [%#lx] = %#lx", at, *reinterpret_cast<DWORD*>(at));
            at = *reinterpret_cast<DWORD*>(at);
        }
        int count = n ? atoi(n) : 8;
        wchar_t text[1024];
        int len = swprintf_s(text, L"Peek %#lx:", at);
        for (int i = 0; i < count && i < 64 && len < 1000; ++i) {
            const DWORD* p = reinterpret_cast<const DWORD*>(at) + i;
            len += IsBadReadPtr(p, 4) ? swprintf_s(text + len, 1024 - len, L" ?") : swprintf_s(text + len, 1024 - len, L" %08lx", *p);
        }
        Log(L"%s", text);
        return;
    }
    if (cmd && (_stricmp(cmd, "poke") == 0 || _stricmp(cmd, "pokef") == 0)) {   // research: poke <hex address> <hex dword>
        bool asFloat = _stricmp(cmd, "pokef") == 0;                                 //      pokef <hex address> <float> [...]
        char* a = strtok_s(nullptr, " \t\r\n", &ctx);
        DWORD at = a ? strtoul(a, nullptr, 16) : 0;
        for (char* v; (v = strtok_s(nullptr, " \t\r\n", &ctx)) != nullptr; at += 4) {
            if (IsBadWritePtr((void*)at, 4)) { Log(L"Poke: %#lx can't be written", at); return; }
            if (asFloat) *reinterpret_cast<float*>(at) = (float)atof(v);
            else *reinterpret_cast<DWORD*>(at) = strtoul(v, nullptr, 16);
            Log(L"Poke: %#lx = %S", at, v);
        }
        return;
    }
    if (cmd && _stricmp(cmd, "teleport") == 0) {   // research: teleport <player> <x> <y> <z> - the player's transform and
        char* who = strtok_s(nullptr, " \t\r\n", &ctx);   // its Havok collision shape (phantom) together
        float v[3] = {};
        for (float& f : v) {
            char* t = strtok_s(nullptr, " \t\r\n", &ctx);
            if (!t) return;
            f = (float)atof(t);
        }
        auto list = *reinterpret_cast<char***>(0xbad988);
        int index = who ? atoi(who) : -1;
        if (!list || index < 0 || index >= *reinterpret_cast<int*>(reinterpret_cast<char*>(list) + 0x20)) return;
        char* player = list[index];
        char* transform = *reinterpret_cast<char**>(player + 0x38);
        char* proxy = *reinterpret_cast<char**>(player + 0x104c);
        char* phantom = proxy ? *reinterpret_cast<char**>(proxy + 0x60) : nullptr;
        if (!transform || !phantom) return;
        float* tp = reinterpret_cast<float*>(transform + 0x30);
        float* pp = reinterpret_cast<float*>(phantom + 0xe0);
        float lift = pp[1] - tp[1];   // the shape sits a little above the feet
        pp[0] = v[0]; pp[1] = v[1] + lift; pp[2] = v[2];
        tp[0] = v[0]; tp[1] = v[1]; tp[2] = v[2];
        Log(L"Teleport: player %d to (%.2f %.2f %.2f)", index, v[0], v[1], v[2]);
        return;
    }
    if (cmd && _stricmp(cmd, "watch") == 0) {   // research: which code writes there (up to 4 hex addresses)
        while (char* p = strtok_s(nullptr, " \t\r\n", &ctx))
            WatchAddress(reinterpret_cast<void*>(strtoul(p, nullptr, 16)), L"the watched address");
        return;
    }
    if (cmd && _stricmp(cmd, "watchread") == 0) {   // research: which code reads or writes there
        while (char* p = strtok_s(nullptr, " \t\r\n", &ctx))
            WatchAddress(reinterpret_cast<void*>(strtoul(p, nullptr, 16)), L"the watched address", true);
        return;
    }
    if (cmd && _stricmp(cmd, "spymark") == 0) {   // research: each player's method calls since the last mark (player spy)
        char* label = strtok_s(nullptr, " \t\r\n", &ctx);
        ResearchSpyMark(label ? label : "");
        return;
    }
    if (cmd && _stricmp(cmd, "watchaim") == 0) {   // research: which code writes a player's aim point
        char* p = strtok_s(nullptr, " \t\r\n", &ctx);
        NetplayWatchAim(p ? atoi(p) : 0);
        return;
    }
    if (cmd && _stricmp(cmd, "stacks") == 0) {  // research: what the game's threads are doing
        NetplayLogStacks();
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
    if (cmd && _stricmp(cmd, "hold") == 0) {   // research: hold <pad> <ms> [lx=.. ly=.. rx=.. ry=.. lt=.. rt=..] [buttons]
        char* padStr = strtok_s(nullptr, " \t\r\n", &ctx);
        char* ms = strtok_s(nullptr, " \t\r\n", &ctx);
        if (!padStr || !ms) return;
        DWORD pad = (DWORD)atoi(padStr) & 3;
        Hold h = {GetTickCount64() + (ULONGLONG)atoi(ms)};
        while (char* t = strtok_s(nullptr, " \t\r\n", &ctx)) {
            char* eq = strchr(t, '=');
            if (!eq) {
                if (WORD bit = ButtonFromName(t)) h.buttons |= bit;
                else Log(L"VPad: unknown button \"%S\"", t);
                continue;
            }
            *eq = 0;
            int v = atoi(eq + 1);
            SHORT stick = (SHORT)max(-32768, min(32767, v));
            BYTE trig = (BYTE)max(0, min(255, v));
            if (!_stricmp(t, "lx")) h.lx = stick; else if (!_stricmp(t, "ly")) h.ly = stick;
            else if (!_stricmp(t, "rx")) h.rx = stick; else if (!_stricmp(t, "ry")) h.ry = stick;
            else if (!_stricmp(t, "lt")) h.lt = trig; else if (!_stricmp(t, "rt")) h.rt = trig;
            else Log(L"VPad: unknown hold value \"%S\"", t);
        }
        std::lock_guard<std::mutex> lock(g_lock);
        g_padActive[pad] = true;
        g_hold[pad] = h;
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
        {"xinput1_3.dll", MAKEINTRESOURCEA(3), (void*)Hook_XInputSetState, (void**)&Real_XInputSetState},
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
