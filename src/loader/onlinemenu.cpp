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

// The Store turns its character itself, every frame while it's on (0x66e5f0, store flag 0xbadcec): by the
// controller's stick, or in keyboard mode straight from the keyboard - Left / Right arrows, else A / D (the
// keyboard's stick). The ONLINE screens show that character with the Store on, so A / D / the arrows turned it
// (and the character's own controls being off didn't matter). There that turning is skipped; Q / E and dragging
// it with the mouse are ours (they set its facing directly).
//   0x66e70a  cmp byte ptr [0xbadcec], 0 / 0x66e711 xorps xmm1, xmm1 / 0x66e714 je 0x66e803 (no store turning)
volatile unsigned char g_noStoreTurn = 0;
extern "C" unsigned char __cdecl StoreTurnBlocked() { return g_open && !NetplayEnabled() ? 1 : 0; }

__declspec(naked) void StoreTurnStub() {
    __asm {
        pushad
        call StoreTurnBlocked
        mov g_noStoreTurn, al
        popad
        cmp g_noStoreTurn, 0
        jne skip
        cmp byte ptr ds:[0xbadcec], 0
        push 0x66e711          // (xorps leaves the flags: its je decides as before)
        ret
    skip:
        xorps xmm1, xmm1
        push 0x66e803
        ret
    }
}

void PatchStoreTurn() {
    static bool done = false;
    if (done) return;
    done = true;
    static const unsigned char expected[] = {0x80, 0x3d, 0xec, 0xdc, 0xba, 0x00, 0x00, 0x0f, 0x57, 0xc9};
    auto at = reinterpret_cast<unsigned char*>(0x66e70a);
    if (memcmp(at, expected, sizeof(expected)) != 0) {
        Log(L"Online: the Store's turning code isn't what we expect - the preview can still be turned with keys");
        return;
    }
    DWORD old;
    VirtualProtect(at, 7, PAGE_EXECUTE_READWRITE, &old);
    at[0] = 0xE9;
    *reinterpret_cast<int*>(at + 1) = (int)(reinterpret_cast<uintptr_t>(&StoreTurnStub) - (0x66e70a + 5));
    at[5] = at[6] = 0x90;
    VirtualProtect(at, 7, old, &old);
    FlushInstructionCache(GetCurrentProcess(), at, 7);
}

}  // namespace

void InitOnlineMenu() { PatchStoreTurn(); }   // (at start: the code isn't running yet; offline it does as before)

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

// Test pipe "yaw": the preview's facing into the log.
void OnlineMenuLogYaw() {
    unsigned char* p;
    float* yaw = PreviewYaw(&p);
    Log(L"Online: preview player %p, facing %s%.4f (at %p), screens %s", p, yaw ? L"" : L"(none) ", yaw ? *yaw : 0.0f,
        yaw, g_open ? L"open" : L"closed");
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

// The preview's own controls switched off on the ONLINE screens: its input object ([player +0x101c],
// zPlayerInputHuman) answers every key / button / stick question "not pressed" while its +0x38 is set (every one
// of those queries checks it first, 0x5bd310...). A / D / the arrows walked it a step before (put back a moment
// later - you saw the step). The menus read the keyboard elsewhere; Q / E turn it directly (OnlineMenuTurnPreview).
unsigned char* g_blockedInput = nullptr;
unsigned char g_blockedWas = 0;
void BlockPreviewInput(unsigned char* player) {
    unsigned char* input = player ? *reinterpret_cast<unsigned char**>(player + 0x101c) : nullptr;
    if (input && IsBadWritePtr(input + 0x38, 1)) input = nullptr;
    if (input != g_blockedInput) {
        if (g_blockedInput && !IsBadWritePtr(g_blockedInput + 0x38, 1)) g_blockedInput[0x38] = g_blockedWas;
        g_blockedInput = input;
        if (input) {
            g_blockedWas = input[0x38];
            Log(L"Online: the preview's own controls off");
        }
    }
    if (input) input[0x38] = 1;
}

// Every frame on the ONLINE screens: undo the Store's own small turns (A / D / arrows / the stick).
void HoldPreview() {
    unsigned char* p;
    float* yaw = PreviewYaw(&p);
    BlockPreviewInput(yaw ? p : nullptr);
    if (!yaw) {
        g_yawPlayer = nullptr;
        return;
    }
    if (p != g_yawPlayer) {
        g_yawPlayer = p;
        g_yaw = *yaw;
        return;
    }
    static int undone = 0;
    // its place too: A / D (the left stick) can walk it as well
    auto xform = *reinterpret_cast<unsigned char**>(p + 0x38);
    float* pos = xform && !IsBadWritePtr(xform + 0x30, 12) ? reinterpret_cast<float*>(xform + 0x30) : nullptr;
    static float held[3];
    static unsigned char* heldFor = nullptr;
    if (pos) {
        if (heldFor != p) {
            heldFor = p;
            held[0] = pos[0]; held[1] = pos[1]; held[2] = pos[2];
        } else {
            float dx = pos[0] - held[0], dz = pos[2] - held[2];
            if (dx * dx + dz * dz < 1.0f) {   // walked a little: back (more: placed somewhere new - fine)
                if ((dx != 0 || dz != 0) && ++undone <= 10)
                    Log(L"Online: the preview walked (%.3f, %.3f) - put back", dx, dz);
                pos[0] = held[0];
                pos[2] = held[2];
            } else {
                held[0] = pos[0]; held[1] = pos[1]; held[2] = pos[2];
            }
        }
    }
    if (*yaw == g_yaw) return;
    float d = std::fmod(*yaw - g_yaw, 6.2831853f);
    if (d > 3.14159265f) d -= 6.2831853f;
    if (d < -3.14159265f) d += 6.2831853f;
    if (std::fabs(d) < 0.35f) {   // the Store turning it: no
        if (++undone <= 10) Log(L"Online: the preview turned %.3f - put back", d);
        *yaw = g_yaw;
    } else {
        g_yaw = *yaw;             // a new character / reset: fine
    }
}

}  // namespace

// d3d9.cpp, every frame: Q / E held on the ONLINE screens turn the preview (about half a turn a second).
void OnlineMenuFrame() {
    if (!g_open || NetplayEnabled()) {
        if (g_blockedInput) BlockPreviewInput(nullptr);   // (left the ONLINE screens: its controls as they were)
        return;
    }
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
