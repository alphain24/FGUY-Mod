// Keyboard and mouse in online sessions - the game's own controls, exactly as offline.
//
// How the game does keyboard / mouse (FGUY.exe, see RESEARCH_NOTES.md): it's one more "controller". Each
// controller slot (ConsolePad0..19) has a device; slot 0 is the keyboard / mouse device (type 8), the others
// XInput pads. Every tick the slot's Update (0x49bb90) copies the device state's current half to its previous
// half and polls: for keyboard / mouse, 0x450930(state, 0) - keys from the game window's key table
// ([0xad39e8]+0xc2, by Windows key code), mouse movement / wheel / buttons / cursor from the raw poll
// 0x4b6ad0 (DirectInput into globals at 0xaefad4..0xaefaf4). A player's input object (player+0x101c,
// zPlayerInputHuman) reads its device (input+0x30): its key bindings, its mouse aiming - all the game's code.
//
// Online every PC runs every player, so each keyboard player gets a device of their own: every tick, on every
// PC, the raw keyboard / mouse state that player's PC sent (KbmRaw, in their inputs) is put where the game's
// poll reads it, the game's own Update steps run on that player's device, and their input object points at
// it. The game's global "keyboard and mouse" switch (0xacc4cd, which some gameplay code checks) is turned on
// on every PC at the same tick, once the level's players exist. Nothing reads this PC's real keyboard / mouse
// directly (dinput.cpp keeps DirectInput quiet; the raw poll is ours in sessions).
//
// Who plays with keyboard and mouse: the session's picks (char:costume:team:input, input k = keyboard), from
// the lobby. Test knob: FGUYMOD_TEST_KBMSLOTS=0,1 (slots).

#include "common.h"

#include <cstring>
#include <cwchar>

void HookMenuInput();
bool KbmMenuKeys(BYTE keys[32]);

namespace {

bool g_session = false;            // an online session with keyboard players
bool g_kbmSlot[4] = {};
HWND g_wnd = nullptr;
volatile LONG g_dx = 0, g_dy = 0, g_dz = 0;   // raw mouse movement / wheel since the last capture
bool g_clipped = false;

// ---- addresses in FGUY.exe ---------------------------------------------------------------------------
const uintptr_t kRawPoll = 0x4b6ad0;        // bool __cdecl RawPoll(): DirectInput -> the globals below
const unsigned char kRawPollStart[] = {0x55, 0x8b, 0xec, 0x83, 0xec, 0x2c, 0x56, 0x57};
const uintptr_t kKbmPoll = 0x450930;        // bool __cdecl Poll(state*, int deviceIndex)
const uintptr_t kWindow = 0xad39e8;         // the game window object; its key table at +0xc2 (256 bytes)
// Menus (the pause menu...) read the keyboard from the keyboard's controller device: its key array (by key
// code, device+0x484, "down" = 0x80) for Esc, Enter and the arrows, in each movie's input update
// (0x5a0280, void __thiscall Update(movie, arg)). The game fills that array from the window's key table,
// which it also empties whenever the window isn't in front - so in a session we write the keyboard players'
// keys for this tick into it right before the update (the same on every PC, in front or not).
const uintptr_t kMenuInputUpdate = 0x5a0280;
const unsigned char kMenuInputUpdateStart[] = {0x81, 0xec, 0xb0, 0x00, 0x00, 0x00, 0x53, 0x55};  // sub esp,b0; push ebx; push ebp
const uintptr_t kKeyboardController = 0xbad7f0;   // the keyboard's controller index
const uintptr_t kControllerTable = 0xaefacc, kControllerCount = 0xaf35ec;
const int kDeviceKeys = 0x484, kDeviceEnabled = 0x6d0;
// The window's "released" notice to its listeners, which the window procedure sends for WM_KEYUP (the key
// code), the mouse button ups (0x100 left, 0x101 right, 0x102 middle) and the wheel (0x104 up, 0x105
// down). A session keeps those messages from the game window (only this PC would see them - a mouse
// wheel weapon switch on one PC only), so KbmApply sends them from everyone's input instead.
const uintptr_t kWindowReleased = 0x4c9180;   // void __thiscall (code)
const unsigned char kWindowReleasedStart[] = {0x55, 0x8b, 0xec, 0x56, 0x8b, 0xb1, 0xd0, 0x01, 0x00, 0x00};
const int kWindowKeys = 0xc2;
const uintptr_t kKbmFlag = 0xacc4cd;        // keyboard / mouse mode
const uintptr_t kPlayerInputVtable = 0xa1a29c;
const int kPlayerInput = 0x101c, kInputDevice = 0x30;
volatile LONG* const gMouseDx = reinterpret_cast<volatile LONG*>(0xaefae4);
volatile LONG* const gMouseDy = reinterpret_cast<volatile LONG*>(0xaefae0);
volatile LONG* const gMouseDz = reinterpret_cast<volatile LONG*>(0xaefadc);
volatile LONG* const gMouseSumX = reinterpret_cast<volatile LONG*>(0xaefaf4);
volatile LONG* const gMouseSumY = reinterpret_cast<volatile LONG*>(0xaefaf0);
volatile LONG* const gMouseSumZ = reinterpret_cast<volatile LONG*>(0xaefaec);
volatile LONG* const gMouseButtons = reinterpret_cast<volatile LONG*>(0xaefae8);
volatile LONG* const gCursorX = reinterpret_cast<volatile LONG*>(0xaefad8);
volatile LONG* const gCursorY = reinterpret_cast<volatile LONG*>(0xaefad4);

const int kDeviceSize = 0x6dc;              // devices sit 0x6dc apart; the state is at +8
unsigned char* g_device[4];                 // one keyboard / mouse device per keyboard player

// ---- the raw poll: ours in sessions --------------------------------------------------------------------
const KbmRaw* g_feed = nullptr;             // set while we poll a keyboard player's device
KbmRaw g_menuKeys;                          // every keyboard player's keys this tick (DirectInput keyboard)
bool g_menuKeysLive = false;
KbmRaw g_menuFeed;                          // the game's own keyboard slot (menus, "click to start" prompts):
bool g_menuFeedReady = false;               //   all keyboard players together, from the previous tick

using MenuInputUpdate_t = void(__fastcall*)(void* movie, void* edx, int arg);
MenuInputUpdate_t g_menuInputOriginal = nullptr;
// A player's cursor (0..32767 of their window) in this PC's window, client pixels.
POINT CursorClient(const KbmRaw& raw) {
    RECT r = {0, 0, 1, 1};
    if (g_wnd) GetClientRect(g_wnd, &r);
    POINT p = {MulDiv(raw.cursorX, r.right, 32767), MulDiv(raw.cursorY, r.bottom, 32767)};
    return p;
}

bool __cdecl Hook_RawPoll() {
    if (!g_feed && g_menuFeedReady) {       // the game's own slot polls at the start of each tick
        g_feed = &g_menuFeed;
        g_menuFeedReady = false;
        bool r = Hook_RawPoll();
        g_feed = nullptr;
        return r;
    }
    if (!g_feed) return false;
    *gMouseDx = g_feed->dx;
    *gMouseDy = g_feed->dy;
    *gMouseDz = g_feed->dz;
    *gMouseSumX += g_feed->dx;
    *gMouseSumY += g_feed->dy;
    *gMouseSumZ += g_feed->dz;
    *gMouseButtons = g_feed->buttons;
    POINT c = CursorClient(*g_feed);
    DisplayClientToGame(&c);
    *gCursorX = c.x;
    *gCursorY = c.y;
    return true;
}

bool PatchJump(uintptr_t at, const unsigned char* expected, size_t n, void* to) {
    if (IsBadReadPtr((void*)at, n) || memcmp((void*)at, expected, n) != 0) {
        Log(L"Input: code at %p isn't what we expect (different game version?) - keyboard online off", (void*)at);
        return false;
    }
    DWORD old;
    VirtualProtect((void*)at, 5, PAGE_EXECUTE_READWRITE, &old);
    auto b = reinterpret_cast<unsigned char*>(at);
    b[0] = 0xE9;
    *reinterpret_cast<int*>(b + 1) = (int)(reinterpret_cast<uintptr_t>(to) - (at + 5));
    VirtualProtect((void*)at, 5, old, &old);
    FlushInstructionCache(GetCurrentProcess(), (void*)at, 5);
    return true;
}

// The game's Update for a controller slot (0x49bb90), for our own devices: current -> previous, then poll.
void SetWindowKeys(const KbmRaw& raw) {
    auto window = *reinterpret_cast<unsigned char**>(kWindow);
    if (window && !IsBadWritePtr(window + kWindowKeys, 0x100))
        for (int k = 0; k < 256; ++k) window[kWindowKeys + k] = (raw.keys[k >> 3] >> (k & 7)) & 1 ? 0x80 : 0;
}

void UpdateDevice(unsigned char* device, const KbmRaw& raw) {
    unsigned char* st = device + 8;
    auto i32 = [st](int off) -> LONG& { return *reinterpret_cast<LONG*>(st + off); };
    i32(0x464) = i32(0x44c);
    i32(0x468) = i32(0x450);
    i32(0x46c) = i32(0x454);
    i32(0x470) = i32(0x458);
    i32(0x474) = i32(0x45c);
    i32(0x478) = i32(0x460);
    memcpy(st + 0x57c, st + 0x47c, 0x100);
    SetWindowKeys(raw);  // this player's keys where the poll copies them from (the window's key table)
    g_feed = &raw;
    using Poll_t = bool(__cdecl*)(unsigned char* state, int index);
    reinterpret_cast<Poll_t>(kKbmPoll)(st, 0);
    g_feed = nullptr;
    i32(0) = 8;  // the state's device type: keyboard / mouse (what the player input asks)
}

}  // namespace

// netplay.cpp, before the first tick: does this session have keyboard players? (picks char:costume:team:input)
void KbmSessionStart(const char* session) {
    char test[32];
    if (GetEnvironmentVariableA("FGUYMOD_TEST_KBMSLOTS", test, sizeof(test))) {
        for (char* c = test; *c; ++c)
            if (*c >= '0' && *c <= '3') g_kbmSlot[*c - '0'] = true;
    } else if (session) {
        const char* picks = strchr(session, '|');
        int slot = 0;
        for (const char* p = picks ? picks + 1 : ""; *p && slot < 4; ++slot) {
            const char* end = strchr(p, ';');
            size_t len = end ? (size_t)(end - p) : strlen(p);
            int colons = 0;
            const char* last = p;
            for (size_t i = 0; i < len; ++i)
                if (p[i] == ':') { ++colons; last = p + i + 1; }
            if (colons >= 3 && *last == 'k') g_kbmSlot[slot] = true;
            p = end ? end + 1 : p + len;
        }
    }
    for (int s = 0; s < 4; ++s) g_session |= g_kbmSlot[s];
    if (!g_session) return;
    if (!PatchJump(kRawPoll, kRawPollStart, sizeof(kRawPollStart), (void*)Hook_RawPoll)) {
        g_session = false;
        return;
    }
    HookMenuInput();
    for (int s = 0; s < 4; ++s)
        if (g_kbmSlot[s]) {
            g_device[s] = static_cast<unsigned char*>(VirtualAlloc(nullptr, 0x1000, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
            *reinterpret_cast<LONG*>(g_device[s]) = 1;      // connected
            *reinterpret_cast<LONG*>(g_device[s] + 8) = 8;  // keyboard / mouse
        }
    Log(L"Input: keyboard and mouse players: %s%s%s%s(the game's own controls)", g_kbmSlot[0] ? L"1 " : L"",
        g_kbmSlot[1] ? L"2 " : L"", g_kbmSlot[2] ? L"3 " : L"", g_kbmSlot[3] ? L"4 " : L"");
}

void __fastcall Hook_MenuInputUpdate(void* movie, void* edx, int arg) {
    BYTE keys[32];
    int index = *reinterpret_cast<int*>(kKeyboardController);
    auto table = *reinterpret_cast<unsigned char***>(kControllerTable);
    if (KbmMenuKeys(keys) && table && index >= 0 && index < *reinterpret_cast<int*>(kControllerCount)) {
        unsigned char* device = table[index];
        if (device && *reinterpret_cast<int*>(device + kDeviceEnabled) && !IsBadWritePtr(device + kDeviceKeys, 256))
            for (int k = 0; k < 256; ++k) device[kDeviceKeys + k] = (keys[k >> 3] >> (k & 7)) & 1 ? 0x80 : 0;
    }
    g_menuInputOriginal(movie, edx, arg);
}

// Session start: the menu input hook (a trampoline with the function's first 8 bytes).
void HookMenuInput() {
    static bool done = false;
    if (done) return;
    done = true;
    auto at = reinterpret_cast<unsigned char*>(kMenuInputUpdate);
    if (IsBadReadPtr(at, sizeof(kMenuInputUpdateStart)) || memcmp(at, kMenuInputUpdateStart, sizeof(kMenuInputUpdateStart))) {
        Log(L"Input: menu input code isn't what we expect - menus won't hear online keyboards");
        return;
    }
    auto t = static_cast<unsigned char*>(VirtualAlloc(nullptr, 32, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
    memcpy(t, kMenuInputUpdateStart, sizeof(kMenuInputUpdateStart));
    t[8] = 0xE9;
    *reinterpret_cast<int*>(t + 9) = (int)((kMenuInputUpdate + 8) - (reinterpret_cast<uintptr_t>(t) + 13));
    g_menuInputOriginal = reinterpret_cast<MenuInputUpdate_t>(t);
    PatchJump(kMenuInputUpdate, kMenuInputUpdateStart, sizeof(kMenuInputUpdateStart), (void*)Hook_MenuInputUpdate);
}

// The session is over for this game (back to the lobby): the game's own keyboard and mouse again.
void KbmLeaveSession() {
    if (!g_session) return;
    g_session = false;
    // (keyboard mode stays as it is: the menus switch it to this player's own controls with the game's
    // CTRL_SetKeyboardMouseInputEnabled, which also moves the menus' controller over - session.as2js)
    g_menuKeysLive = false;
    g_menuFeedReady = false;
    DWORD old;   // the raw poll's own first bytes back
    VirtualProtect((void*)kRawPoll, sizeof(kRawPollStart), PAGE_EXECUTE_READWRITE, &old);
    memcpy((void*)kRawPoll, kRawPollStart, sizeof(kRawPollStart));
    VirtualProtect((void*)kRawPoll, sizeof(kRawPollStart), old, &old);
    FlushInstructionCache(GetCurrentProcess(), (void*)kRawPoll, sizeof(kRawPollStart));
    if (g_wnd) ClipCursor(nullptr);
    Log(L"Input: keyboard and mouse straight to the game again");
}

// Every keyboard player's keys for this tick (the same on every PC) - what the menus read in a session.
// False before keyboard mode is on.
bool KbmMenuKeys(BYTE keys[32]) {
    if (!g_session || !g_menuKeysLive) return false;
    memcpy(keys, g_menuKeys.keys, 32);
    return true;
}

// The game's own cursor reads (menus: the pause menu's pointer) in a session: the keyboard players' cursor
// for this tick (the same spot in every PC's window). False before keyboard mode is on.
bool KbmCursorScreen(POINT* pt) {
    if (!g_session || !g_menuKeysLive || !g_wnd) return false;
    *pt = CursorClient(g_menuKeys);
    return ClientToScreen(g_wnd, pt) != FALSE;
}

bool KbmIsKeyboardSlot(int slot) { return slot >= 0 && slot < 4 && g_kbmSlot[slot]; }

// Back in the lobby after a match (session.as2js, right after the game's own CTRL_SetKeyboardMouseInputEnabled
// with this player's controls): a controller player whose pad the game didn't find there would stay in
// keyboard mode - controller mode anyway (what this game ran in before the session).
void KbmBackInLobby(bool keyboard) {
    volatile BYTE& flag = *reinterpret_cast<volatile BYTE*>(kKbmFlag);
    if (!keyboard && flag) {
        flag = 0;
        Log(L"Input: back in the lobby - no controller found for the game's switch, controller mode anyway");
    }
    Log(L"Input: back in the lobby with %s (keyboard mode %d, menu controllers %#x)",
        keyboard ? L"keyboard and mouse" : L"a controller", (int)flag, *reinterpret_cast<DWORD*>(0xade6a8));
}

// display.cpp: the game's main window exists.
void KbmInit(HWND wnd) { g_wnd = wnd; }

// display.cpp's window procedure. Returns true if the message was ours.
bool KbmOnMessage(HWND wnd, UINT msg, WPARAM wp, LPARAM lp, LRESULT* result) {
    if (!NetplayEnabled()) return false;
    // In a session this PC's keys reach the game only through the lockstep (as a keyboard player's input):
    // the game window doesn't see them (Windows still handles Alt+Tab, Alt+F4 and the like).
    // (Mouse button ups and the wheel too: menus hear them from the window procedure - KbmApply tells them.)
    if (msg == WM_KEYDOWN || msg == WM_KEYUP || msg == WM_SYSKEYDOWN || msg == WM_SYSKEYUP || msg == WM_CHAR ||
        msg == WM_SYSCHAR || msg == WM_DEADCHAR || msg == WM_LBUTTONUP || msg == WM_RBUTTONUP ||
        msg == WM_MBUTTONUP || msg == WM_MOUSEWHEEL) {
        *result = DefWindowProcW(wnd, msg, wp, lp);
        return true;
    }
    if (!g_session) return false;
    if (msg == WM_INPUT) {
        RAWINPUT in;
        UINT size = sizeof(in);
        if (GetRawInputData((HRAWINPUT)lp, RID_INPUT, &in, &size, sizeof(RAWINPUTHEADER)) != (UINT)-1 &&
            in.header.dwType == RIM_TYPEMOUSE) {
            if (!(in.data.mouse.usFlags & MOUSE_MOVE_ABSOLUTE)) {
                InterlockedExchangeAdd(&g_dx, in.data.mouse.lLastX);
                InterlockedExchangeAdd(&g_dy, in.data.mouse.lLastY);
            }
            if (in.data.mouse.usButtonFlags & RI_MOUSE_WHEEL) InterlockedExchangeAdd(&g_dz, (SHORT)in.data.mouse.usButtonData);
        }
        *result = DefWindowProcW(wnd, msg, wp, lp);
        return true;
    }
    return false;
}

// netplay.cpp, while a scene holds the game: Enter / Space / Esc skip it too.
bool KbmSkipPressed() {
    bool front = g_wnd && GetForegroundWindow() == g_wnd;
    return front && ((GetAsyncKeyState(VK_RETURN) | GetAsyncKeyState(VK_SPACE) | GetAsyncKeyState(VK_ESCAPE)) & 0x8000);
}

namespace {
// Raw mouse input goes to one window per program; the game's DirectInput mouse (quiet in sessions) takes it
// over when it starts, so we claim it back now and then.
void ClaimRawMouse() {
    RAWINPUTDEVICE now[8];
    UINT n = 8;
    if (GetRegisteredRawInputDevices(now, &n, sizeof(RAWINPUTDEVICE)) != (UINT)-1)
        for (UINT i = 0; i < n; ++i)
            if (now[i].usUsagePage == 0x01 && now[i].usUsage == 0x02 && now[i].hwndTarget == g_wnd && !now[i].dwFlags) return;
    RAWINPUTDEVICE mouse{0x01, 0x02, 0, g_wnd};
    BOOL ok = RegisterRawInputDevices(&mouse, 1, sizeof(mouse));
    static int logged = 0;
    if (logged++ < 3) Log(L"Input: mouse movement %s", ok ? L"claimed for the game window" : L"could not be claimed");
}

SHORT Clamp16(LONG v) { return (SHORT)(v > 32767 ? 32767 : (v < -32768 ? -32768 : v)); }

// Test input (vpad.cpp's pipe: kbm key / mouse / click): pretend keys are held and the mouse moves, without
// touching the real keyboard and mouse. Merged into my capture.
CRITICAL_SECTION g_testLock;
bool g_testInit = false;
ULONGLONG g_testKeyUntil[256];
LONG g_testDx = 0, g_testDy = 0;
int g_testMoveTicks = 0;
ULONGLONG g_testClickUntil = 0;
int g_testCursorX = -1, g_testCursorY = -1;   // test: the cursor, 0..32767 of the window
}  // namespace

void KbmTestCommand(const char* what, int a, int b, int c) {
    if (!g_testInit) { InitializeCriticalSection(&g_testLock); g_testInit = true; }
    EnterCriticalSection(&g_testLock);
    ULONGLONG now = GetTickCount64();
    if (!strcmp(what, "key") && a > 0 && a < 256) g_testKeyUntil[a] = now + (b > 0 ? b : 100);
    else if (!strcmp(what, "mouse")) { g_testDx = a; g_testDy = b; g_testMoveTicks = c > 0 ? c : 1; }
    else if (!strcmp(what, "click")) g_testClickUntil = now + (a > 0 ? a : 100);
    else if (!strcmp(what, "cursor")) { g_testCursorX = a; g_testCursorY = b; }
    LeaveCriticalSection(&g_testLock);
    Log(L"Input: test %S %d %d %d", what, a, b, c);
}

// netplay.cpp, once per tick (for the tick being sent): my keyboard and mouse right now.
void KbmCapture(KbmRaw* out) {
    memset(out, 0, sizeof(*out));
    LONG dx = InterlockedExchange(&g_dx, 0), dy = InterlockedExchange(&g_dy, 0), dz = InterlockedExchange(&g_dz, 0);
    static ULONGLONG lastClaim = 0;
    if (g_wnd && GetTickCount64() - lastClaim > 2000) {
        lastClaim = GetTickCount64();
        ClaimRawMouse();
    }
    bool front = g_wnd && GetForegroundWindow() == g_wnd;
    if (front) {
        RECT r;
        GetClientRect(g_wnd, &r);
        MapWindowPoints(g_wnd, nullptr, reinterpret_cast<POINT*>(&r), 2);
        ClipCursor(&r);  // keep the mouse in the window while playing
        g_clipped = true;
    } else if (g_clipped) {
        ClipCursor(nullptr);
        g_clipped = false;
    }
    if (g_testInit) {   // test input counts as "in front"
        EnterCriticalSection(&g_testLock);
        ULONGLONG now = GetTickCount64();
        for (int k = 1; k < 256; ++k)
            if (g_testKeyUntil[k] > now) out->keys[k >> 3] |= (BYTE)(1 << (k & 7));
        if (g_testMoveTicks > 0) { dx += g_testDx; dy += g_testDy; --g_testMoveTicks; }
        if (g_testClickUntil > now) out->buttons |= 1;
        if (g_testCursorX >= 0) { out->cursorX = Clamp16(g_testCursorX); out->cursorY = Clamp16(g_testCursorY); }
        LeaveCriticalSection(&g_testLock);
        if (!front) { out->dx = Clamp16(dx); out->dy = Clamp16(dy); return; }
    }
    if (!front) return;   // not in front: nothing held
    for (int k = 1; k < 256; ++k)
        if (GetAsyncKeyState(k) & 0x8000) out->keys[k >> 3] |= (BYTE)(1 << (k & 7));
    out->dx = Clamp16(dx);
    out->dy = Clamp16(dy);
    out->dz = Clamp16(dz);
    const int buttons[5] = {VK_LBUTTON, VK_RBUTTON, VK_MBUTTON, VK_XBUTTON1, VK_XBUTTON2};
    for (int b = 0; b < 5; ++b)
        if (GetAsyncKeyState(buttons[b]) & 0x8000) out->buttons |= (BYTE)(1 << b);
    POINT c;
    RECT r;
    if (GetCursorPos(&c) && ScreenToClient(g_wnd, &c) && GetClientRect(g_wnd, &r) && r.right > 0 && r.bottom > 0) {
        out->cursorX = Clamp16(MulDiv(max(0L, min(c.x, r.right)), 32767, r.right));
        out->cursorY = Clamp16(MulDiv(max(0L, min(c.y, r.bottom)), 32767, r.bottom));
    }
}

// netplay.cpp, at the start of every tick, with every player's inputs for this tick: give each keyboard
// player their device, updated by the game's own code from their keyboard and mouse.
void KbmApply(const KbmRaw* bySlot, int players) {
    if (!g_session) return;
    // Keyboard mode starts once every player's level HUD is up - told through the inputs, so it's the same
    // tick on every PC. (Before that the session autopilot drives the menus with controller presses, and the
    // level setup would otherwise stop for a "click anywhere to start".)
    if (!NetplayLevelLive()) return;
    unsigned char* list[8];
    int n = GamePlayers(list, 8);
    bool inLevel = false;  // a player with a view of their own: the level runs (menus: the session autopilot
    for (int i = 0; i < n; ++i)   // drives them with controller presses)
        if (*reinterpret_cast<int*>(list[i] + 0xdfc) >= 0) inLevel = true;
    if (!inLevel) return;
    *reinterpret_cast<volatile BYTE*>(kKbmFlag) = 1;  // the game's keyboard / mouse mode (same tick on every PC)
    KbmRaw menu = {};
    bool first = true;
    for (int s = 0; s < players && s < 4; ++s) {
        if (!g_kbmSlot[s] || !g_device[s]) continue;
        const KbmRaw& r = bySlot[s];
        for (int k = 0; k < 32; ++k) menu.keys[k] |= r.keys[k];
        menu.dx = (SHORT)(menu.dx + r.dx);
        menu.dy = (SHORT)(menu.dy + r.dy);
        menu.dz = (SHORT)(menu.dz + r.dz);
        menu.buttons |= r.buttons;
        if (first) { menu.cursorX = r.cursorX; menu.cursorY = r.cursorY; first = false; }
        UpdateDevice(g_device[s], r);
        for (int i = 0; i < n; ++i) {
            if (*reinterpret_cast<int*>(list[i] + 0xdf8) != s) continue;
            auto input = *reinterpret_cast<unsigned char**>(list[i] + kPlayerInput);
            if (input && !IsBadWritePtr(input, kInputDevice + 4) &&
                *reinterpret_cast<uintptr_t*>(input) == kPlayerInputVtable)
                *reinterpret_cast<unsigned char**>(input + kInputDevice) = g_device[s];
        }
    }
    static KbmRaw before = {};   // the window's "released" notices (see kWindowReleased), same tick everywhere
    static bool canNotify = !IsBadReadPtr((void*)kWindowReleased, sizeof(kWindowReleasedStart)) &&
        memcmp((void*)kWindowReleased, kWindowReleasedStart, sizeof(kWindowReleasedStart)) == 0;
    auto window = *reinterpret_cast<void**>(kWindow);
    if (canNotify && window) {
        using Released_t = void(__fastcall*)(void* window, void* edx, int code);
        auto released = reinterpret_cast<Released_t>(kWindowReleased);
        for (int k = 1; k < 256; ++k)
            if (((before.keys[k >> 3] >> (k & 7)) & 1) && !((menu.keys[k >> 3] >> (k & 7)) & 1)) released(window, nullptr, k);
        const int buttonCodes[3] = {0x100, 0x101, 0x102};
        for (int b = 0; b < 3; ++b)
            if ((before.buttons >> b & 1) && !(menu.buttons >> b & 1)) released(window, nullptr, buttonCodes[b]);
        if (menu.dz > 0) released(window, nullptr, 0x104);
        if (menu.dz < 0) released(window, nullptr, 0x105);
    }
    before = menu;
    // the game's own keyboard slot (polled at the start of the next tick): everyone with keyboard together
    g_menuFeed = menu;
    g_menuFeedReady = true;
    g_menuKeys = menu;
    g_menuKeysLive = true;
    SetWindowKeys(menu);
}
