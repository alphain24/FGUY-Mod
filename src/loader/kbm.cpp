// Keyboard and mouse in online sessions.
//
// In a session every player is one controller (netplay.cpp sends each player's pad state to everyone), so
// the keyboard and mouse are turned into this player's controller: keys press its buttons, WASD is the
// left stick, mouse movement is the right stick (camera), the buttons and wheel are triggers and buttons.
// That keeps every PC seeing the same inputs. A real controller works at the same time.
//
//   WASD move, Shift sprint (left stick click), mouse camera, left click shoot (RT), right click zoom /
//   detonate (LT), Space jump (A), R reload (X), F or V melee (B), E or the wheel change weapon (Y),
//   G grenade (RB), Q special ability / item (LB), Esc pause (Start), Tab objectives (Back),
//   1 / up switch character, 2 / 3 / left / right secondary items, T / down taunt (d-pad);
//   menus: arrows, Enter (A), Backspace (B).
// Only while the game window is in front. The mouse is read as raw input and the cursor is kept inside the
// window and hidden there.
//
// Settings ([Input] in FGUYMod.ini): MouseSensitivity=1.0, InvertMouse=0.

#include "common.h"

#include <xinput.h>

#include <cmath>
#include <cwchar>

namespace {

bool g_on = false;
HWND g_wnd = nullptr;
volatile LONG g_dx = 0, g_dy = 0;   // raw mouse movement since the last tick
volatile LONG g_wheel = 0;          // wheel notches since the last tick
int g_wheelTicks = 0;               // ticks left to hold the change-weapon button for a wheel notch
bool g_clipped = false;
float g_sensitivity = 1.0f;
bool g_invert = false;

// Full right-stick deflection at about this many mouse counts per tick (1/60 s), sensitivity 1.0.
const float kCountsForFull = 40.0f;  // (a 1000 Hz mouse moved briskly gives about that much)
const float kDeadZone = 8700.0f;     // the game ignores small stick movements; mouse moves start past it

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
            if (in.data.mouse.usButtonFlags & RI_MOUSE_WHEEL) InterlockedIncrement(&g_wheel);
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

// netplay.cpp, while a scene holds the game: a key that skips it.
bool KbmSkipPressed() {
    return g_on && g_wnd && GetForegroundWindow() == g_wnd && (Down(VK_SPACE) || Down(VK_RETURN) || Down(VK_ESCAPE));
}

// netplay.cpp, once per tick: add the keyboard and mouse to this player's controller state.
void KbmMerge(WORD* buttons, BYTE* lt, BYTE* rt, SHORT* lx, SHORT* ly, SHORT* rx, SHORT* ry) {
    if (!g_on) return;
    LONG dx = InterlockedExchange(&g_dx, 0), dy = InterlockedExchange(&g_dy, 0);
    LONG wheel = InterlockedExchange(&g_wheel, 0);
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
    static ULONGLONG lastClaim = 0;
    if (GetTickCount64() - lastClaim > 2000) {
        lastClaim = GetTickCount64();
        ClaimRawMouse();
    }
    if (!front) { g_wheelTicks = 0; return; }

    WORD b = 0;
    if (Down(VK_SPACE) || Down(VK_RETURN)) b |= XINPUT_GAMEPAD_A;
    if (Down('F') || Down('V') || Down(VK_BACK)) b |= XINPUT_GAMEPAD_B;
    if (Down('R')) b |= XINPUT_GAMEPAD_X;
    if (Down('E')) b |= XINPUT_GAMEPAD_Y;
    if (Down('Q')) b |= XINPUT_GAMEPAD_LEFT_SHOULDER;
    if (Down('G')) b |= XINPUT_GAMEPAD_RIGHT_SHOULDER;
    if (Down(VK_ESCAPE)) b |= XINPUT_GAMEPAD_START;
    if (Down(VK_TAB)) b |= XINPUT_GAMEPAD_BACK;
    if (Down(VK_SHIFT)) b |= XINPUT_GAMEPAD_LEFT_THUMB;
    if (Down(VK_UP) || Down('1')) b |= XINPUT_GAMEPAD_DPAD_UP;
    if (Down(VK_DOWN) || Down('T')) b |= XINPUT_GAMEPAD_DPAD_DOWN;
    if (Down(VK_LEFT) || Down('2')) b |= XINPUT_GAMEPAD_DPAD_LEFT;
    if (Down(VK_RIGHT) || Down('3')) b |= XINPUT_GAMEPAD_DPAD_RIGHT;
    if (wheel) g_wheelTicks = 4;     // a wheel notch: tap change weapon
    if (g_wheelTicks > 0) { b |= XINPUT_GAMEPAD_Y; --g_wheelTicks; }
    *buttons |= b;
    if (Down(VK_LBUTTON)) *rt = 255;
    if (Down(VK_RBUTTON)) *lt = 255;

    int mx = (Down('D') ? 1 : 0) - (Down('A') ? 1 : 0), my = (Down('W') ? 1 : 0) - (Down('S') ? 1 : 0);
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
