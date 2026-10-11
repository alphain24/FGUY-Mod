// The online pause menu: in an online match, pause (Start / Esc) opens a small menu of your own while the
// match keeps running for everyone (your character stands still meanwhile): RESUME, QUIT TO LOBBY, and for
// the host QUIT WITH PARTY. Once every player has it open, the game pauses for real - the same tick on every
// PC: we press pause for the first player still in the match, and the game's own pause menu takes over
// (synced; its Resume unpauses everyone, its Quit Game takes everyone back to the lobby).
//
// The menu is drawn by the HUD movie (src/ui/pause.as2js, through the HUD patch), which asks
// FGUYMOD_PauseMenu / FGUYMOD_PauseCursor every frame. The logic is here, fed from each tick's local input
// (netplay.cpp SampleLocal) - the player's own controller and keyboard / mouse - before it's sent.
// Whether a player has the menu open travels with their input (KbmRaw.flags bit 1); the host asking everyone
// to quit is bit 2.

#include "common.h"

#include <xinput.h>

#include <cstdio>
#include <cstring>
#include <string>

namespace {

enum Item { kResume, kQuit, kPartyQuit };
bool g_open = false;
int g_sel = 0;
bool g_gamePaused = false;     // we paused the game for everyone (its own pause menu is up)
int g_partyTicks = 0;          // host: "everyone quit" goes out with the next few inputs
volatile LONG g_leave = 0;     // QUIT TO LOBBY chosen: leave the session (from the menu's poll, outside a tick)
volatile LONG g_quit = 0;      // the HUD movie should quit the level now (GAME_Quit)
int g_openCount = 0, g_players = 0;
int g_injectSlot = -1, g_injectTicks = 0;
WORD g_lastButtons = 0;
BYTE g_lastKeys[32];
BYTE g_lastMouse = 0;
SHORT g_lastLy = 0;
SHORT g_cursorX = 0, g_cursorY = 0;
bool g_keyboard = false;
std::string g_text;

bool Key(const BYTE* keys, int vk) { return (keys[vk >> 3] >> (vk & 7)) & 1; }

int Items() { return NetplayLocalSlot() == 0 ? 3 : 2; }

// The menu's items, as fractions of the screen (pause.as2js draws them at the same place).
const float kItemLeft = 0.36f, kItemRight = 0.64f, kItemTop = 0.43f, kItemStep = 0.085f, kItemHeight = 0.07f;

int ItemAt(SHORT cx, SHORT cy) {
    float x = cx / 32767.0f, y = cy / 32767.0f;
    if (x < kItemLeft || x > kItemRight) return -1;
    for (int i = 0; i < Items(); ++i) {
        float top = kItemTop + i * kItemStep;
        if (y >= top && y <= top + kItemHeight) return i;
    }
    return -1;
}

void Choose(int item) {
    if (item == kResume) {
        g_open = false;
    } else if (item == kQuit) {
        g_open = false;
        InterlockedExchange(&g_leave, 1);
        Log(L"Pause: quit to the lobby");
    } else if (item == kPartyQuit && NetplayLocalSlot() == 0) {
        g_partyTicks = 12;
        Log(L"Pause: the host takes everyone back to the lobby");
    }
}

}  // namespace

// netplay.cpp, every tick, with this PC's input for the tick being sent: run the menu and hide the input
// that belongs to it. Returns true while the menu is open (the input is then sent empty).
bool OnlinePauseSample(WORD* buttons, SHORT* ly, KbmRaw* kbm, bool keyboardPlayer) {
    if (!NetplayLevelLive()) return false;
    g_keyboard = keyboardPlayer;
    const BYTE* keys = kbm->keys;
    WORD down = *buttons & ~g_lastButtons;
    auto pressed = [&](int vk) { return keyboardPlayer && Key(keys, vk) && !Key(g_lastKeys, vk); };
    bool click = keyboardPlayer && (kbm->buttons & 1) && !(g_lastMouse & 1);
    bool stickUp = *ly > 20000 && g_lastLy <= 20000, stickDown = *ly < -20000 && g_lastLy >= -20000;
    g_lastButtons = *buttons;
    memcpy(g_lastKeys, keys, 32);
    g_lastMouse = kbm->buttons;
    g_lastLy = *ly;
    if (keyboardPlayer) { g_cursorX = kbm->cursorX; g_cursorY = kbm->cursorY; }

    bool pausePress = (down & XINPUT_GAMEPAD_START) || pressed(VK_ESCAPE);
    if (g_gamePaused) return false;   // the game's own pause menu is up: everything goes to it
    if (!g_open) {
        if (pausePress) {
            g_open = true;
            g_sel = 0;
            Log(L"Pause: menu open (the match goes on)");
        } else {
            if (g_partyTicks > 0) { --g_partyTicks; kbm->flags |= 4; }
            *buttons &= ~XINPUT_GAMEPAD_START;   // (only the menu answers the pause button online)
            if (keyboardPlayer) kbm->keys[VK_ESCAPE >> 3] &= ~(1 << (VK_ESCAPE & 7));
            return false;
        }
    } else {
        int n = Items();
        if ((down & XINPUT_GAMEPAD_DPAD_UP) || stickUp || pressed(VK_UP) || pressed('W')) g_sel = (g_sel + n - 1) % n;
        if ((down & XINPUT_GAMEPAD_DPAD_DOWN) || stickDown || pressed(VK_DOWN) || pressed('S')) g_sel = (g_sel + 1) % n;
        if (keyboardPlayer) {
            int at = ItemAt(kbm->cursorX, kbm->cursorY);
            static SHORT lastX = -1, lastY = -1;
            if (at >= 0 && (kbm->cursorX != lastX || kbm->cursorY != lastY)) g_sel = at;   // hover
            lastX = kbm->cursorX;
            lastY = kbm->cursorY;
            if (click && at >= 0) Choose(at);
        }
        if ((down & XINPUT_GAMEPAD_A) || pressed(VK_RETURN) || pressed(VK_SPACE)) Choose(g_sel);
        else if ((down & (XINPUT_GAMEPAD_B | XINPUT_GAMEPAD_START)) || pressed(VK_ESCAPE) || pressed(VK_BACK)) g_open = false;
    }
    BYTE flags = kbm->flags;
    if (g_partyTicks > 0) { --g_partyTicks; flags |= 4; }
    KbmRaw empty = {};                   // the menu's input stays here: the character stands still
    *kbm = empty;
    kbm->flags = flags | (g_open ? 2 : 0);
    *buttons = 0;
    *ly = 0;
    return true;
}

// netplay.cpp, every tick on every PC, with everyone's input: all menus open -> pause the game (returns the
// slot to press pause for, and for how many ticks); the host's "everyone quit".
int OnlinePauseTick(const BYTE* flags, const bool* gone, int players, bool* keyboard) {
    int open = 0, here = 0, first = -1;
    for (int s = 0; s < players; ++s) {
        if (gone[s]) continue;
        ++here;
        if (first < 0) first = s;
        if (flags[s] & 2) ++open;
        if (s == 0 && (flags[s] & 4) && !InterlockedExchange(&g_quit, 1))
            Log(L"Pause: the host is taking everyone back to the lobby");
    }
    g_openCount = open;
    g_players = here;
    if (!g_gamePaused && here > 0 && open == here) {
        g_gamePaused = true;
        g_open = false;
        g_injectSlot = first;
        g_injectTicks = 4;
        Log(L"Pause: everyone has the menu open - pausing the game");
    }
    if (g_injectTicks > 0) {
        --g_injectTicks;
        if (keyboard) *keyboard = KbmIsKeyboardSlot(g_injectSlot);
        return g_injectSlot;
    }
    return -1;
}

// ui.cpp: a menu command ran (GAME_Unpause: the game's pause menu resumed - the same tick everywhere).
void OnlinePauseCommand(const char* command) {
    if (g_gamePaused && command && strcmp(command, "GAME_Unpause") == 0) {
        g_gamePaused = false;
        Log(L"Pause: the game resumed");
    }
}

// FGUYMOD_PauseMenu, every frame from the HUD: "open|selected|host|open count|players|quit".
const char* OnlinePauseMenu() {
    if (InterlockedExchange(&g_leave, 0)) {
        NetplayLeave(L"quit to the lobby");
        InterlockedExchange(&g_quit, 1);
    }
    char buf[64];
    sprintf_s(buf, "%d|%d|%d|%d|%d|%d", g_open ? 1 : 0, g_sel, NetplayLocalSlot() == 0 ? 1 : 0, g_openCount, g_players,
              g_quit == 1 ? 1 : 0);
    g_text = buf;
    return g_text.c_str();
}

// FGUYMOD_PauseCursor: "x|y|show" (x, y in 1/10000 of the screen) - keyboard / mouse players get a pointer.
const char* OnlinePauseCursor() {
    static char buf[32];
    sprintf_s(buf, "%d|%d|%d", g_cursorX * 10000 / 32767, g_cursorY * 10000 / 32767, g_open && g_keyboard ? 1 : 0);
    return buf;
}

// FGUYMOD_PauseQuitDone: the HUD has quit the level.
void OnlinePauseQuitDone() { InterlockedExchange(&g_quit, 2); }
