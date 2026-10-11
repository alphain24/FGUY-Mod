// Keyboard and mouse helpers for the ONLINE screens (src/ui/lobby.as2js), and the test bot.
//
// * Typing: in keyboard mode the game turns W A S D into menu directions and drops the other letters before
//   the menu sees them, so the ONLINE screens get typed text from here instead: key presses in the game
//   window while the screens are open (display.cpp's window procedure), read with FGUYMOD_TypedText.
//   FGUYMOD_FromWASD tells a menu direction that came from W A S D (not the arrows or a controller).
// * The 3D character preview (the Store's model, menu player 2) turns with a mouse drag on it
//   (FGUYMOD_TurnPreview) and while Q / E are held (every frame, here). Its facing is [player+0x5c]+0x78
//   (what PLYR_ResetRotation resets, FGUY.exe 0x69c6e1). The Store itself also turns it with the left
//   stick, which in keyboard mode is A / D and the arrow keys - the keys that move around the menus - so
//   every frame small turns we didn't make are undone (big jumps are a new character: kept).
// * Test bot (FGUYMOD_BOT=1, "FGUYMod\Start test bot.bat"): a second copy of the game that stays hidden
//   and muted, joins the first open lobby (or FGUYMOD_BOT_JOIN=<code>), readies up and plays on its own.

#include "common.h"

#include <cmath>
#include <mutex>
#include <string>

namespace {

volatile bool g_open = false;   // the ONLINE screens are showing
std::mutex g_typedLock;
std::string g_typed;
HWND g_menuWnd = nullptr;
unsigned char* g_yawPlayer = nullptr;  // the preview's facing as we last left it
float g_yaw = 0;

unsigned char* PreviewPlayer() {
    unsigned char* list[8];
    int n = GamePlayers(list, 8);
    for (int i = 0; i < n; ++i)
        if (*reinterpret_cast<int*>(list[i] + 0xdf8) == 2) return list[i];
    return nullptr;
}

}  // namespace

void OnlineMenuOpen(bool open) {
    g_open = open;
    std::lock_guard<std::mutex> lock(g_typedLock);
    g_typed.clear();
}

// display.cpp's window procedure: a key went down in the game window.
void OnlineMenuKey(HWND wnd, WPARAM vk) {
    g_menuWnd = wnd;
    if (!g_open || (GetKeyState(VK_CONTROL) & 0x8000) || (GetKeyState(VK_MENU) & 0x8000)) return;
    char c = 0;
    if (vk >= 'A' && vk <= 'Z') c = (char)vk;
    else if (vk >= '0' && vk <= '9') c = (char)vk;
    else if (vk >= VK_NUMPAD0 && vk <= VK_NUMPAD9) c = (char)('0' + (vk - VK_NUMPAD0));
    else if (vk == VK_BACK) c = '\b';
    if (!c) return;
    std::lock_guard<std::mutex> lock(g_typedLock);
    if (g_typed.size() < 32) g_typed += c;
}

// FGUYMOD_TypedText: what was typed since the last call (letters, digits, \b for Backspace).
const char* OnlineMenuTyped() {
    static std::string out;
    std::lock_guard<std::mutex> lock(g_typedLock);
    out = g_typed;
    g_typed.clear();
    return out.c_str();
}

// FGUYMOD_FromWASD(direction key 37-40): the menu direction came from W A S D, not an arrow key or a pad.
bool OnlineMenuFromWASD(int key) {
    static const int letter[4] = {'A', 'W', 'D', 'S'};  // 37 left, 38 up, 39 right, 40 down
    static const int arrow[4] = {VK_LEFT, VK_UP, VK_RIGHT, VK_DOWN};
    if (key < 37 || key > 40) return false;
    int i = key - 37;
    return (GetAsyncKeyState(letter[i]) & 0x8000) && !(GetAsyncKeyState(arrow[i]) & 0x8000);
}

float* PreviewYaw(unsigned char** player) {
    unsigned char* p = PreviewPlayer();
    if (player) *player = p;
    if (!p) return nullptr;
    auto body = *reinterpret_cast<unsigned char**>(p + 0x5c);
    if (!body || IsBadWritePtr(body + 0x78, 4)) return nullptr;
    return reinterpret_cast<float*>(body + 0x78);
}

// FGUYMOD_TurnPreview(radians): turn the 3D character preview.
void OnlineMenuTurnPreview(float radians) {
    unsigned char* p;
    float* yaw = PreviewYaw(&p);
    if (!yaw) return;
    if (p != g_yawPlayer) {
        g_yawPlayer = p;
        g_yaw = *yaw;
    }
    const float kTwoPi = 6.2831853f;
    g_yaw += radians;
    if (g_yaw > kTwoPi) g_yaw -= kTwoPi;
    if (g_yaw < -kTwoPi) g_yaw += kTwoPi;
    *yaw = g_yaw;
}

namespace {

// Every frame on the ONLINE screens: undo the Store's own small turns (A / D / arrows / the stick).
void HoldPreview() {
    unsigned char* p;
    float* yaw = PreviewYaw(&p);
    if (!yaw) {
        g_yawPlayer = nullptr;
        return;
    }
    if (p != g_yawPlayer) {
        g_yawPlayer = p;
        g_yaw = *yaw;
        return;
    }
    if (*yaw == g_yaw) return;
    float d = std::fmod(*yaw - g_yaw, 6.2831853f);
    if (d > 3.14159265f) d -= 6.2831853f;
    if (d < -3.14159265f) d += 6.2831853f;
    if (std::fabs(d) < 0.35f) *yaw = g_yaw;  // the Store turning it: no
    else g_yaw = *yaw;                       // a new character / reset: fine
}

}  // namespace

// d3d9.cpp, every frame: Q / E held on the ONLINE screens turn the preview (about half a turn a second).
void OnlineMenuFrame() {
    if (!g_open || NetplayEnabled()) return;
    HoldPreview();
    if (!g_menuWnd || GetForegroundWindow() != g_menuWnd) return;
    static LARGE_INTEGER last = {}, freq = {};
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    if (!freq.QuadPart) QueryPerformanceFrequency(&freq);
    float dt = last.QuadPart ? (float)(now.QuadPart - last.QuadPart) / freq.QuadPart : 0.0f;
    last = now;
    if (dt > 0.1f) dt = 0.1f;
    int dir = ((GetAsyncKeyState('E') & 0x8000) ? 1 : 0) - ((GetAsyncKeyState('Q') & 0x8000) ? 1 : 0);
    if (dir) OnlineMenuTurnPreview(dir * 3.0f * dt);
}

// ---- the test bot -----------------------------------------------------------------------------------------
bool IsBot() {
    static int bot = -1;
    if (bot < 0) {
        wchar_t v[8];
        bot = GetEnvironmentVariableW(L"FGUYMOD_BOT", v, 8) ? 1 : 0;
    }
    return bot == 1;
}

// The test bot and test copies (FGUYMOD_MUTE=1, tools\start_test.ps1) play no sound at all.
bool StaysMuted() {
    static int muted = -1;
    if (muted < 0) {
        wchar_t v[8];
        muted = IsBot() || GetEnvironmentVariableW(L"FGUYMOD_MUTE", v, 8) ? 1 : 0;
    }
    return muted == 1;
}

const char* BotJoinCode() {
    static char code[16];
    if (!GetEnvironmentVariableA("FGUYMOD_BOT_JOIN", code, sizeof(code))) code[0] = 0;
    return code;
}
