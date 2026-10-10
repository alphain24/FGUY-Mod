// Keyboard and mouse in online sessions - with the game's own controls.
//
// In a session every player is one controller (netplay.cpp sends each player's pad state to everyone; the
// game's own keyboard / mouse support can only drive player 1 of the PC it runs on). So each of the game's
// keyboard actions is played as the controller input that does the same thing, using the player's own key
// bindings (the game's Options > Controls > Keyboard mapping, saved in the registry as KeyConfig): pairs of
// <action, key>, keys being Windows key codes, or 0x100.. for the mouse buttons and wheel.
//
//   action (default key)                     controller
//   17/15/18/16 move fwd/left/back/right     left stick
//    8 Jump (Space)          A               43 Melee Attack (F)        B
//   21 Reload/Interact (R)   X               27 Weapon Wheel (Mouse 3)  Y (held)
//   29/30 Next / Previous Weapon (wheel)     Y (tapped)
//   23 Shoot Weapon (Mouse 1)  RT            25 Zoom/Detonate (Mouse 2)  LT
//   37 Throw Grenade (G)     RB              22 Special Ability/Item (Q)  LB
//   20 Sprint (Shift)        left stick click   52 Center Camera (C)  right stick click
//   51 Switch Character (E)  d-pad up        48 Taunt (T)               d-pad down
//   40 Toggle Item Forward (Z), 42 Item Wheel (Ctrl)  d-pad right       41 Toggle Item Backward (X)  d-pad left
//   54 Objectives (Tab)      Back            10 Pause Menu (Esc)         Start
//   Quick Weapon 1-6 (31-36): no controller equivalent.
// Mouse movement is the camera (right stick). In menus: arrows, Enter (A) and Backspace (B) work too.
// Only while the game window is in front; the mouse is read as raw input and the cursor stays in the window.
//
// Settings ([Input] in FGUYMod.ini): MouseSensitivity=1.0, InvertMouse=0.

#include "common.h"

#include <xinput.h>

#include <cmath>
#include <cwchar>
#include <vector>

namespace {

bool g_on = false;
HWND g_wnd = nullptr;
volatile LONG g_dx = 0, g_dy = 0;           // raw mouse movement since the last tick
volatile LONG g_wheelUp = 0, g_wheelDown = 0;  // wheel notches since the last tick
int g_tapTicks = 0;                          // ticks left of a wheel notch's change-weapon tap
bool g_clipped = false;
float g_sensitivity = 1.0f;
bool g_invert = false;

// Full right-stick deflection at about this many mouse counts per tick (1/60 s), sensitivity 1.0.
const float kCountsForFull = 40.0f;  // (a 1000 Hz mouse moved briskly gives about that much)
const float kDeadZone = 8700.0f;     // the game ignores small stick movements; mouse moves start past it

// ---- the game's key bindings --------------------------------------------------------------------------
enum Target { kButton, kLeftTrigger, kRightTrigger, kMoveX, kMoveY, kWheelTap };
struct Action { int id; Target target; int value; };  // value: button bits, or the stick direction
const Action kActions[] = {
    {17, kMoveY, 1}, {18, kMoveY, -1}, {15, kMoveX, -1}, {16, kMoveX, 1},
    {8, kButton, XINPUT_GAMEPAD_A}, {43, kButton, XINPUT_GAMEPAD_B}, {21, kButton, XINPUT_GAMEPAD_X},
    {27, kButton, XINPUT_GAMEPAD_Y}, {29, kWheelTap, 0}, {30, kWheelTap, 0},
    {23, kRightTrigger, 0}, {25, kLeftTrigger, 0},
    {37, kButton, XINPUT_GAMEPAD_RIGHT_SHOULDER}, {22, kButton, XINPUT_GAMEPAD_LEFT_SHOULDER},
    {20, kButton, XINPUT_GAMEPAD_LEFT_THUMB}, {52, kButton, XINPUT_GAMEPAD_RIGHT_THUMB},
    {51, kButton, XINPUT_GAMEPAD_DPAD_UP}, {48, kButton, XINPUT_GAMEPAD_DPAD_DOWN},
    {40, kButton, XINPUT_GAMEPAD_DPAD_RIGHT}, {42, kButton, XINPUT_GAMEPAD_DPAD_RIGHT},
    {41, kButton, XINPUT_GAMEPAD_DPAD_LEFT},
    {54, kButton, XINPUT_GAMEPAD_BACK}, {10, kButton, XINPUT_GAMEPAD_START},
};
// The game's defaults (what an untouched KeyConfig holds).
const int kDefaults[][2] = {
    {17, 'W'}, {15, 'A'}, {18, 'S'}, {16, 'D'}, {8, VK_SPACE}, {23, 0x100}, {25, 0x101}, {37, 'G'}, {21, 'R'},
    {43, 'F'}, {20, VK_LSHIFT}, {48, 'T'}, {51, 'E'}, {22, 'Q'}, {54, VK_TAB}, {52, 'C'}, {27, 0x102},
    {29, 0x105}, {30, 0x104}, {42, VK_CONTROL}, {40, 'Z'}, {41, 'X'}, {10, VK_ESCAPE},
};
const int kMouse1 = 0x100, kMouse2 = 0x101, kMouse3 = 0x102, kWheelUp = 0x104, kWheelDown = 0x105;

struct Binding { int action, key; };
std::vector<Binding> g_bindings;

void LoadBindings() {
    std::vector<Binding> b;
    HKEY key;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\Heavy Iron Studios Inc.\\Family Guy\\Settings", 0, KEY_READ,
                      &key) == ERROR_SUCCESS) {
        BYTE data[1024];
        DWORD size = sizeof(data), type = 0;
        if (RegQueryValueExW(key, L"KeyConfig", nullptr, &type, data, &size) == ERROR_SUCCESS)
            for (DWORD o = 0; o + 8 <= size; o += 8) {
                Binding one;
                memcpy(&one.action, data + o, 4);
                memcpy(&one.key, data + o + 4, 4);
                b.push_back(one);
            }
        RegCloseKey(key);
    }
    bool fromGame = !b.empty();
    if (!fromGame)
        for (auto& d : kDefaults) b.push_back({d[0], d[1]});
    static bool logged = false;
    if (!logged) {
        logged = true;
        Log(L"Input: %zu key bindings from %s", b.size(), fromGame ? L"the game's keyboard mapping" : L"the game's defaults");
    }
    g_bindings = b;
}

// Is a bound key held (or was a bound wheel direction turned this tick)?
bool KeyDown(int key, LONG up, LONG down) {
    switch (key) {
        case kMouse1: return (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0;
        case kMouse2: return (GetAsyncKeyState(VK_RBUTTON) & 0x8000) != 0;
        case kMouse3: return (GetAsyncKeyState(VK_MBUTTON) & 0x8000) != 0;
        case kWheelUp: return up > 0;
        case kWheelDown: return down > 0;
        default: return key > 0 && key < 0x100 && (GetAsyncKeyState(key) & 0x8000) != 0;
    }
}

bool Down(int vk) { return (GetAsyncKeyState(vk) & 0x8000) != 0; }

// Raw mouse input goes to one window per program; the game's DirectInput mouse (silent in sessions,
// dinput.cpp) takes it over when it starts. Make sure it's ours.
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

SHORT StickFromMouse(LONG counts, bool flip) {
    if (!counts) return 0;
    float v = counts / kCountsForFull * g_sensitivity;
    if (flip) v = -v;
    float a = std::fabs(v);
    if (a > 1) a = 1;
    float out = kDeadZone + a * (32767.0f - kDeadZone);
    return (SHORT)(v < 0 ? -out : out);
}

}  // namespace

// display.cpp: the game's main window exists.
void KbmInit(HWND wnd) {
    if (!NetplayEnabled() || g_on) return;
    g_wnd = wnd;
    wchar_t sens[16];
    SettingString(L"Input", L"MouseSensitivity", nullptr, L"1.0", sens, 16);
    g_sensitivity = (float)_wtof(sens);
    if (g_sensitivity <= 0) g_sensitivity = 1.0f;
    g_invert = SettingInt(L"Input", L"InvertMouse", nullptr, 0) != 0;
    LoadBindings();
    g_on = true;   // the raw mouse is claimed once the game runs (KbmMerge)
    Log(L"Input: keyboard and mouse play as this player's controller (mouse sensitivity %.2f%s)", g_sensitivity,
        g_invert ? L", inverted" : L"");
}

// display.cpp's window procedure. Returns true if the message was ours.
bool KbmOnMessage(HWND wnd, UINT msg, WPARAM wp, LPARAM lp, LRESULT* result) {
    if (!g_on) return false;
    if (msg == WM_INPUT) {
        RAWINPUT in;
        UINT size = sizeof(in);
        if (GetRawInputData((HRAWINPUT)lp, RID_INPUT, &in, &size, sizeof(RAWINPUTHEADER)) != (UINT)-1 &&
            in.header.dwType == RIM_TYPEMOUSE) {
            if (!(in.data.mouse.usFlags & MOUSE_MOVE_ABSOLUTE)) {
                InterlockedExchangeAdd(&g_dx, in.data.mouse.lLastX);
                InterlockedExchangeAdd(&g_dy, in.data.mouse.lLastY);
            }
            if (in.data.mouse.usButtonFlags & RI_MOUSE_WHEEL) {
                if ((SHORT)in.data.mouse.usButtonData > 0) InterlockedIncrement(&g_wheelUp);
                else InterlockedIncrement(&g_wheelDown);
            }
        }
        *result = DefWindowProcW(wnd, msg, wp, lp);
        return true;
    }
    if (msg == WM_SETCURSOR && LOWORD(lp) == HTCLIENT && GetForegroundWindow() == wnd) {
        SetCursor(nullptr);  // no pointer over the game while it's played with the mouse
        *result = TRUE;
        return true;
    }
    return false;
}

// netplay.cpp, while a scene holds the game: a key that skips it (Jump / Pause as bound, or Enter).
bool KbmSkipPressed() {
    if (!g_on || !g_wnd || GetForegroundWindow() != g_wnd) return false;
    if (Down(VK_RETURN)) return true;
    for (auto& b : g_bindings)
        if ((b.action == 8 || b.action == 10) && KeyDown(b.key, 0, 0)) return true;
    return false;
}

// netplay.cpp, once per tick: add the keyboard and mouse to this player's controller state.
void KbmMerge(WORD* buttons, BYTE* lt, BYTE* rt, SHORT* lx, SHORT* ly, SHORT* rx, SHORT* ry) {
    if (!g_on) return;
    LONG dx = InterlockedExchange(&g_dx, 0), dy = InterlockedExchange(&g_dy, 0);
    LONG up = InterlockedExchange(&g_wheelUp, 0), down = InterlockedExchange(&g_wheelDown, 0);
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
    static ULONGLONG lastCheck = 0;
    if (GetTickCount64() - lastCheck > 2000) {  // and pick up rebinding in the game's Options
        lastCheck = GetTickCount64();
        ClaimRawMouse();
        LoadBindings();
    }
    if (!front) { g_tapTicks = 0; return; }

    WORD b = 0;
    int mx = 0, my = 0;
    bool tap = false;
    for (auto& bind : g_bindings) {
        if (!KeyDown(bind.key, up, down)) continue;
        for (auto& a : kActions) {
            if (a.id != bind.action) continue;
            switch (a.target) {
                case kButton: b |= (WORD)a.value; break;
                case kLeftTrigger: *lt = 255; break;
                case kRightTrigger: *rt = 255; break;
                case kMoveX: mx += a.value; break;
                case kMoveY: my += a.value; break;
                case kWheelTap: tap = true; break;
            }
        }
    }
    if (tap) g_tapTicks = 4;   // a wheel notch: tap change weapon
    if (g_tapTicks > 0) { b |= XINPUT_GAMEPAD_Y; --g_tapTicks; }
    // menus: arrows, Enter, Backspace
    if (Down(VK_UP)) b |= XINPUT_GAMEPAD_DPAD_UP;
    if (Down(VK_DOWN)) b |= XINPUT_GAMEPAD_DPAD_DOWN;
    if (Down(VK_LEFT)) b |= XINPUT_GAMEPAD_DPAD_LEFT;
    if (Down(VK_RIGHT)) b |= XINPUT_GAMEPAD_DPAD_RIGHT;
    if (Down(VK_RETURN)) b |= XINPUT_GAMEPAD_A;
    if (Down(VK_BACK)) b |= XINPUT_GAMEPAD_B;
    *buttons |= b;

    mx = mx > 0 ? 1 : (mx < 0 ? -1 : 0);
    my = my > 0 ? 1 : (my < 0 ? -1 : 0);
    if (mx || my) {
        float k = (mx && my) ? 0.7071f : 1.0f;  // diagonals as fast as straight lines
        *lx = (SHORT)(mx * k * 32767);
        *ly = (SHORT)(my * k * 32767);
    }
    // mouse reports and game ticks don't line up exactly: average over two ticks, or the camera stutters
    static LONG prevDx = 0, prevDy = 0;
    LONG sx = (dx + prevDx) / 2, sy = (dy + prevDy) / 2;
    prevDx = dx;
    prevDy = dy;
    if (sx || sy) {
        *rx = StickFromMouse(sx, false);
        *ry = StickFromMouse(sy, !g_invert);  // mouse up = look up
    }
}
