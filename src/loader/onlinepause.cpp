// The online pause menu: in an online match, pause (Start / Esc) opens a small menu of your own while the
// match keeps running for everyone (your character stands still meanwhile): RESUME, SETTINGS (your own, see
// onlinesettings.cpp), QUIT TO LOBBY, and for the host QUIT WITH PARTY. Once every player has it open, the game pauses for real - the same tick on every
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

#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>

namespace {

enum Item { kResume, kSettings, kQuit, kPartyQuit };
bool g_open = false;
int g_sel = 0;
bool g_settings = false;       // the SETTINGS page is up
int g_row = 1, g_top = 0;      //   its selected row, and the first one shown
const int kVisible = 12;       //   rows shown at once
bool g_gamePaused = false;     // we paused the game for everyone (its own pause menu is up)
int g_partyTicks = 0;          // host: "everyone quit" goes out with the next few inputs
volatile LONG g_leave = 0;     // QUIT TO LOBBY chosen: leave the session (from the menu's poll, outside a tick)
volatile LONG g_quit = 0;      // the HUD movie should quit the level now (GAME_Quit)
int g_openCount = 0, g_players = 0;
int g_injectSlot = -1, g_injectTicks = 0;
int g_storyPauseTicks = 0;     // story co-op: my pause goes out with the next few inputs
bool g_swallowBack = false;    // my SETTINGS closed over the game's pause menu: its back press stays mine
volatile bool g_hudTakesStart = false;   // my weapons screen is up (the HUD): START is its own (leaves it), not ours
// The end of the match's rematch vote: Y votes (KbmRaw.flags bit 4, with my input - every PC sees everyone's); once
// everyone has, the host's input picks the menu's first item (RETRY) by itself. The host can still pick either.
ULONGLONG g_resultsAt = 0;     // the end of the match's menu last said it's up (FGUYMOD_MenuMouse "results")
bool g_vote = false;           // mine
bool g_votes[4] = {};          // everyone's (OnlinePauseTick)
int g_votePlayers = 0;
bool g_rematchGo = false, g_rematchDone = false;
bool ResultsShown() { return GetTickCount64() - g_resultsAt < 500; }
WORD g_lastButtons = 0;
BYTE g_lastKeys[32];
BYTE g_lastMouse = 0;
SHORT g_lastLy = 0, g_lastLx = 0;
SHORT g_cursorX = 0, g_cursorY = 0;
bool g_keyboard = false;
std::string g_text;

bool Key(const BYTE* keys, int vk) { return (keys[vk >> 3] >> (vk & 7)) & 1; }

// My keyboard player's death menu and the mouse (FGUYMOD_DeathMouse, the HUD on this PC): where its buttons are
// on this screen (1/10000 of it) and the key each stands for. A click on one presses that key in my input for a
// few ticks - the match's input, so every PC does it the same tick (a click straight on the HUD's button happened
// on this PC only: out of sync). The boxes come from the HUD's thread, the clicks are seen on the game's.
// A box of kind 1 is the weapons screen's wheel: pointing at a weapon holds the W A S D direction of it (the wheel's
// six weapons sit every 60 degrees from the top; a keyboard player chooses with W, W+D, S+D, S, S+A, W+A), a click
// presses the box's key (X: buy). (aspect: the screen's width / height x 1000, for the angle)
// A box of kind 2 is an item of a menu list (FGUYMOD_MenuMouse: the end of the match's RETRY / MAIN MENU, the game's
// pause menu, their yes / no questions): its index there (in the aspect's place) and the item chosen now (cur).
// Pointing at another item moves the choice there with Up / Down (kind 3: a row - Left / Right), a click moves it and
// presses Enter - one key at a time, through the match's input.
struct MouseBox { int x0, y0, x1, y1, vk, kind, aspect, cur; };
SRWLOCK g_boxLock = SRWLOCK_INIT;
MouseBox g_boxes[4];
int g_boxCount = 0;
ULONGLONG g_boxesAt = 0;       // (the HUD says every 33 ms; older than this: the menu is gone)
MouseBox g_listBoxes[6];       // the results movie's list (kind 2)
int g_listCount = 0;
ULONGLONG g_listAt = 0;
int g_clickVk = 0, g_clickTicks = 0;
int g_seq[12], g_seqLen = 0, g_seqPos = 0, g_seqTick = 0;   // keys to press one after another (the list)
ULONGLONG g_hoverAt = 0;
bool g_osPointer = false;      // the Windows pointer is shown for the menus (OnlinePauseCursor)

int BoxKeyAt(SHORT cx, SHORT cy, int* hold = nullptr) {
    int x = cx * 10000 / 32767, y = cy * 10000 / 32767, vk = 0;
    if (hold) *hold = 0;
    AcquireSRWLockShared(&g_boxLock);
    if (GetTickCount64() - g_boxesAt < 300)
        for (int i = 0; i < g_boxCount && !vk; ++i) {
            const MouseBox& b = g_boxes[i];
            if (x < b.x0 || x > b.x1 || y < b.y0 || y > b.y1) continue;
            vk = b.vk;
            if (b.kind == 1 && hold) {   // the wheel: which weapon it points at
                double dx = (x - (b.x0 + b.x1) / 2.0) * (b.aspect > 0 ? b.aspect / 1000.0 : 16.0 / 9.0);
                double dy = y - (b.y0 + b.y1) / 2.0;
                if (dx * dx + dy * dy > 0.04 * ((b.y1 - b.y0) / 2.0) * ((b.y1 - b.y0) / 2.0)) {   // (not its middle)
                    double a = std::atan2(dx, -dy) * 180.0 / 3.14159265358979;
                    if (a < 0) a += 360;
                    static const int keys[6] = {1, 1 | 8, 4 | 8, 4, 4 | 2, 1 | 2};   // W 1, A 2, S 4, D 8
                    *hold = keys[(int)((a + 30) / 60) % 6];
                }
            }
        }
    ReleaseSRWLockShared(&g_boxLock);
    return vk;
}

bool BoxesShown() {
    AcquireSRWLockShared(&g_boxLock);
    ULONGLONG now = GetTickCount64();
    bool on = (g_boxCount > 0 && now - g_boxesAt < 300) || (g_listCount > 0 && now - g_listAt < 300);
    ReleaseSRWLockShared(&g_boxLock);
    return on;
}

// The list item the pointer is on (-1: none), the item chosen now, and whether the list is a row (kind 3).
int ListItemAt(SHORT cx, SHORT cy, int* cur, bool* row) {
    int x = cx * 10000 / 32767, y = cy * 10000 / 32767, item = -1;
    AcquireSRWLockShared(&g_boxLock);
    if (GetTickCount64() - g_listAt < 300 && g_listCount > 0) {   // (the choice now: every box says it)
        *cur = g_listBoxes[0].cur;
        *row = g_listBoxes[0].kind == 3;
    }
    if (GetTickCount64() - g_listAt < 300)
        for (int i = 0; i < g_listCount && item < 0; ++i) {
            const MouseBox& b = g_listBoxes[i];
            if (x >= b.x0 && x <= b.x1 && y >= b.y0 && y <= b.y1) { item = b.aspect; *cur = b.cur; *row = b.kind == 3; }
        }
    ReleaseSRWLockShared(&g_boxLock);
    return item;
}

int ParseBoxes(const char* boxes, MouseBox* b, int max) {
    int n = 0;
    for (const char* at = boxes; at && *at && n < max;) {
        b[n].kind = 0;
        b[n].aspect = 0;
        b[n].cur = 0;
        if (sscanf_s(at, "%d,%d,%d,%d,%d,%d,%d,%d", &b[n].x0, &b[n].y0, &b[n].x1, &b[n].y1, &b[n].vk, &b[n].kind, &b[n].aspect,
                     &b[n].cur) >= 5 &&
            b[n].vk > 0 && b[n].vk < 256)
            ++n;
        at = strchr(at, ';');
        if (at) ++at;
    }
    return n;
}

int Items() { return NetplayLocalSlot() == 0 ? 4 : 3; }

// The menu's items, as fractions of the screen (menudraw.as2js draws them at the same place).
const float kItemLeft = 0.36f, kItemRight = 0.64f, kItemTop = 0.40f, kItemStep = 0.085f, kItemHeight = 0.07f;
// The SETTINGS page's rows (menudraw.as2js too): the value part on the right takes clicks (left half: less).
const float kRowLeft = 0.22f, kRowRight = 0.78f, kRowTop = 0.215f, kRowStep = 0.054f, kRowHeight = 0.048f;
const float kValueLeft = 0.52f, kValueRight = 0.76f;

SettingsRow g_rows[32];
int g_rowCount = 0;

bool Selectable(int r) {
    char k = g_rows[r].kind;
    return k == 's' || k == 't' || k == 'c' || k == 'b';
}

void RefreshRows() { g_rowCount = OnlineSettingsRows(g_rows, 32); }

void MoveRow(int dir) {
    for (int r = g_row + dir; r >= 0 && r < g_rowCount; r += dir)
        if (Selectable(r)) { g_row = r; break; }
    if (g_row < g_top) g_top = g_row;
    if (g_row >= g_top + kVisible) g_top = g_row - kVisible + 1;
    if (dir < 0 && g_top > 0 && !Selectable(g_top - 1) && g_row == g_top) --g_top;   // (its section's title too)
}

int RowAt(SHORT cx, SHORT cy, bool* valueLeft, bool* inValue) {
    float x = cx / 32767.0f, y = cy / 32767.0f;
    if (x < kRowLeft || x > kRowRight) return -1;
    for (int k = 0; k < kVisible && g_top + k < g_rowCount; ++k) {
        float top = kRowTop + k * kRowStep;
        if (y >= top && y <= top + kRowHeight) {
            *inValue = x >= kValueLeft && x <= kValueRight;
            *valueLeft = x < (kValueLeft + kValueRight) / 2;
            return g_top + k;
        }
    }
    return -1;
}

void OpenSettings() {
    g_settings = true;
    RefreshRows();
    g_row = 0;
    g_top = 0;
    MoveRow(1);
    Log(L"Pause: settings");
}

// One row's action: left / right (-1 / +1) or A (0).
void Act(int row, int delta) {
    char k = g_rows[row].kind;
    if (k == 'b') { if (delta == 0) g_settings = false; return; }
    if (k == 's' && delta == 0) return;
    OnlineSettingsChange(row, k == 'c' && delta == 0 ? 1 : delta);
    RefreshRows();
}

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
    } else if (item == kSettings) {
        OpenSettings();
    } else if (item == kQuit) {
        g_open = false;
        InterlockedExchange(&g_leave, 1);
        Log(L"Pause: quit to the lobby");
    } else if (item == kPartyQuit && NetplayLocalSlot() == 0) {
        g_partyTicks = 12;
        Log(L"Pause: the host takes everyone back to the lobby");
    }
}

// The presses this file makes (a click's key, the list's keys, the flags) last a number of 1/60 s steps - real time, not
// samples: my input is sampled every tick, which was 60 a second, but in a free-running match it's every frame (at
// 240 fps a click's Enter was down for 25 ms and the death menu missed it: no respawn). How many steps have gone by since
// the last sample (in lockstep one a tick, as before).
int g_steps = 0;
int StepsSinceLast() {
    static LARGE_INTEGER freq{}, last{};
    static LONGLONG carry = 0;
    if (!freq.QuadPart) QueryPerformanceFrequency(&freq);
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    if (!NetplayFreeRun() || !last.QuadPart || now.QuadPart < last.QuadPart) { last = now; carry = 0; return 1; }
    LONGLONG span = (now.QuadPart - last.QuadPart) * 60 + carry;
    last = now;
    int steps = (int)(span / freq.QuadPart);
    carry = span % freq.QuadPart;
    return steps > 60 ? 60 : steps;
}

// The game's menu lists by mouse (FGUYMOD_MenuMouse's boxes): pointing moves the choice, a click moves it and picks -
// keys into my input, one at a time.
void ListMouse(KbmRaw* kbm, bool click) {
    int cur = 0;
    bool row = false;
    int item = ListItemAt(kbm->cursorX, kbm->cursorY, &cur, &row);
    if (!BoxesShown()) g_seqLen = g_seqPos = 0;   // (the menu went: what's left of the keys isn't for anything now)
    const int next = row ? VK_RIGHT : VK_DOWN, prev = row ? VK_LEFT : VK_UP;
    // Pointing chooses only when the pointer moves: the item it's on then. (Following it all the time, a pointer
    // resting on an item's edge flipped the choice over and over - the chosen item draws bigger, the other smaller,
    // so the pointer was on one, then the other.)
    static SHORT lastX = -1, lastY = -1;
    static int target = -1;
    if (kbm->cursorX != lastX || kbm->cursorY != lastY) target = item;
    if (!BoxesShown()) target = -1;   // (a menu that comes up later: only once the pointer moves over it)
    lastX = kbm->cursorX;
    lastY = kbm->cursorY;
    if (g_seqPos >= g_seqLen && item >= 0 && click) {
        g_seqLen = g_seqPos = g_seqTick = 0;
        for (int k = cur; k != item && g_seqLen < 10; k += item > cur ? 1 : -1) g_seq[g_seqLen++] = item > cur ? next : prev;
        g_seq[g_seqLen++] = VK_RETURN;
        Log(L"Pause: game menu - item %d clicked (chosen %d)", item, cur);
        target = item;
    } else if (g_seqPos >= g_seqLen && target >= 0 && target != cur) {   // (one step, then look again: the menu says
        ULONGLONG now = GetTickCount64();                                  //  where it is)
        if (now >= g_hoverAt) {
            g_seqLen = g_seqPos = g_seqTick = 0;
            g_seq[g_seqLen++] = target > cur ? next : prev;
            g_hoverAt = now + 150;
        }
    }
    if (g_seqPos < g_seqLen) {   // each key down 4 steps (1/15 s), then up 4
        int key = g_seq[g_seqPos];
        if (g_seqTick < 4) kbm->keys[key >> 3] |= (BYTE)(1 << (key & 7));
        g_seqTick += g_steps;
        if (g_seqTick >= 8) { g_seqTick = 0; ++g_seqPos; }
    }
}

}  // namespace

// netplay.cpp, every tick, with this PC's input for the tick being sent: run the menu and hide the input
// that belongs to it. Returns true while the menu is open (the input is then sent empty).
bool OnlinePauseSample(WORD* buttons, SHORT* lx, SHORT* ly, KbmRaw* kbm, bool keyboardPlayer) {
    g_steps = StepsSinceLast();
    if (!NetplayLevelLive()) return false;
    g_keyboard = keyboardPlayer;
    const BYTE* keys = kbm->keys;
    WORD down = *buttons & ~g_lastButtons;
    BYTE before[32];   // last tick's keys (g_lastKeys takes this tick's below)
    memcpy(before, g_lastKeys, 32);
    auto pressed = [&](int vk) { return keyboardPlayer && Key(keys, vk) && !Key(before, vk); };
    bool click = keyboardPlayer && (kbm->buttons & 1) && !(g_lastMouse & 1);
    bool stickUp = *ly > 20000 && g_lastLy <= 20000, stickDown = *ly < -20000 && g_lastLy >= -20000;
    bool stickLeft = *lx < -20000 && g_lastLx >= -20000, stickRight = *lx > 20000 && g_lastLx <= 20000;
    g_lastButtons = *buttons;
    memcpy(g_lastKeys, keys, 32);
    g_lastMouse = kbm->buttons;
    g_lastLy = *ly;
    g_lastLx = *lx;
    if (keyboardPlayer) { g_cursorX = kbm->cursorX; g_cursorY = kbm->cursorY; }

    bool pausePress = (down & XINPUT_GAMEPAD_START) || pressed(VK_ESCAPE);
    bool startToGame = g_hudTakesStart && !g_open;   // (START leaves my weapons screen: it goes to the game, as offline)
    if (startToGame && !pressed(VK_ESCAPE)) pausePress = false;
    if (g_gamePaused) g_storyPauseTicks = 0;   // (my pause did it: the rest of its presses would pause again after RESUME)
    if (g_gamePaused && !(g_open && g_settings)) {   // the game's own pause menu is up: everything goes to it
        if (g_swallowBack) {   // (the Esc / B that closed my SETTINGS: not the game's menu's back too)
            bool held = (*buttons & (XINPUT_GAMEPAD_B | XINPUT_GAMEPAD_START)) || Key(keys, VK_ESCAPE) || Key(keys, VK_BACK);
            if (!held) g_swallowBack = false;
            *buttons &= ~(XINPUT_GAMEPAD_B | XINPUT_GAMEPAD_START);
            kbm->keys[VK_ESCAPE >> 3] &= ~(1 << (VK_ESCAPE & 7));
            kbm->keys[VK_BACK >> 3] &= ~(1 << (VK_BACK & 7));
        }
        if (keyboardPlayer) ListMouse(kbm, click);    //   (its buttons by mouse, if it's mine to drive)
        return false;
    }
    if (g_gamePaused && keyboardPlayer) {   // (my SETTINGS over it: the pointer as on our menu)
        g_cursorX = kbm->cursorX;
        g_cursorY = kbm->cursorY;
    }
    if (keyboardPlayer && !g_open) {   // the death menu's buttons / the weapons wheel by mouse: their keys (BoxKeyAt)
        int hold = 0;
        int vk = BoxKeyAt(kbm->cursorX, kbm->cursorY, &hold);
        static const int dirs[4] = {'W', 'A', 'S', 'D'};
        for (int d = 0; d < 4; ++d)
            if (hold & (1 << d)) kbm->keys[dirs[d] >> 3] |= (BYTE)(1 << (dirs[d] & 7));
        static int lastHold = 0;
        if (hold && hold != lastHold) {
            static int logged = 0;
            if (++logged <= 20) Log(L"Pause: the wheel pointed at - holding %s%s%s%s", hold & 1 ? L"W" : L"", hold & 2 ? L"A" : L"",
                                     hold & 4 ? L"S" : L"", hold & 8 ? L"D" : L"");
        }
        lastHold = hold;
        if (!click) vk = 0;
        bool deathMenu = false;   // (my death menu: its keys held down 1/10 s, as a click's - a quick tap was missed)
        AcquireSRWLockShared(&g_boxLock);
        deathMenu = g_boxCount > 0 && g_boxes[0].kind == 0 && GetTickCount64() - g_boxesAt < 300;
        ReleaseSRWLockShared(&g_boxLock);
        if (!vk && deathMenu && g_clickTicks <= 0) {
            if (pressed(VK_RETURN)) vk = VK_RETURN;
            else if (pressed('X')) vk = 'X';
            else if (pressed('Y')) vk = 'Y';
        }
        if (vk) {
            g_clickVk = vk;
            g_clickTicks = 6;
            Log(L"Pause: menu clicked - key %d", vk);
        }
        if (g_clickTicks > 0) {   // (down 6 steps: 1/10 s)
            g_clickTicks -= g_steps;
            kbm->keys[g_clickVk >> 3] |= (BYTE)(1 << (g_clickVk & 7));
        }
        ListMouse(kbm, click);
    }
    if (ResultsShown()) {
        if (pressed('Y') || (down & XINPUT_GAMEPAD_Y)) {
            g_vote = !g_vote;
            Log(L"Pause: rematch vote %s", g_vote ? L"yes" : L"withdrawn");
        }
        if (g_vote) kbm->flags |= 16;
        if (g_rematchGo && keyboardPlayer && g_seqPos >= g_seqLen) {   // (the host: RETRY, as a click on it would)
            g_rematchGo = false;
            int cur = 0;
            bool row = false;
            AcquireSRWLockShared(&g_boxLock);
            if (g_listCount > 0) { cur = g_listBoxes[0].cur; row = g_listBoxes[0].kind == 3; }
            ReleaseSRWLockShared(&g_boxLock);
            g_seqLen = g_seqPos = g_seqTick = 0;
            for (int k = cur; k > 0 && g_seqLen < 10; --k) g_seq[g_seqLen++] = row ? VK_LEFT : VK_UP;
            g_seq[g_seqLen++] = VK_RETURN;
            Log(L"Pause: everyone voted for a rematch - RETRY");
        }
    } else {
        g_vote = false;
    }
    // Story co-op pauses as it does offline: one player's pause pauses everyone's game, the game's own pause menu
    // (theirs to drive). The press goes out with my input (KbmRaw.flags bit 3) - OnlinePauseTick on every PC.
    if (!g_open && pausePress && NetplayStoryCoop()) {
        g_storyPauseTicks = 4;
        Log(L"Pause: story co-op - pausing the game for everyone");
    }
    if (!g_open) {
        if (pausePress && !NetplayStoryCoop()) {
            g_open = true;
            g_sel = 0;
            g_settings = false;
            Log(L"Pause: menu open (the match goes on)");
        } else {
            if (g_partyTicks > 0) { g_partyTicks -= g_steps; kbm->flags |= 4; }
            if (g_storyPauseTicks > 0) { g_storyPauseTicks -= g_steps; kbm->flags |= 8; }
            if (!startToGame) *buttons &= ~XINPUT_GAMEPAD_START;   // (only the menu answers the pause button online)
            if (keyboardPlayer) kbm->keys[VK_ESCAPE >> 3] &= ~(1 << (VK_ESCAPE & 7));
            return false;
        }
    } else if (g_settings) {
        RefreshRows();   // (rows can change on their own: the display mode once the window has switched)
        bool up = (down & XINPUT_GAMEPAD_DPAD_UP) || stickUp || pressed(VK_UP) || pressed('W') || kbm->dz > 0;
        bool dn = (down & XINPUT_GAMEPAD_DPAD_DOWN) || stickDown || pressed(VK_DOWN) || pressed('S') || kbm->dz < 0;
        bool left = (down & XINPUT_GAMEPAD_DPAD_LEFT) || stickLeft || pressed(VK_LEFT) || pressed('A');
        bool right = (down & XINPUT_GAMEPAD_DPAD_RIGHT) || stickRight || pressed(VK_RIGHT) || pressed('D');
        if (up) MoveRow(-1);
        if (dn) MoveRow(1);
        if (left) Act(g_row, -1);
        if (right) Act(g_row, 1);
        if (keyboardPlayer) {
            bool valueLeft = false, inValue = false;
            int at = RowAt(kbm->cursorX, kbm->cursorY, &valueLeft, &inValue);
            static SHORT lastX = -1, lastY = -1;
            if (at >= 0 && Selectable(at) && (kbm->cursorX != lastX || kbm->cursorY != lastY)) g_row = at;   // hover
            lastX = kbm->cursorX;
            lastY = kbm->cursorY;
            if (click && at >= 0 && Selectable(at)) {
                g_row = at;
                char k = g_rows[at].kind;
                Act(at, k == 't' || k == 'b' ? 0 : inValue ? (valueLeft ? -1 : 1) : 0);
            }
        }
        if ((down & XINPUT_GAMEPAD_A) || pressed(VK_RETURN) || pressed(VK_SPACE)) Act(g_row, 0);
        else if ((down & (XINPUT_GAMEPAD_B | XINPUT_GAMEPAD_START)) || pressed(VK_ESCAPE) || pressed(VK_BACK)) g_settings = false;
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
    if (g_gamePaused && !g_settings && g_open) {   // (my SETTINGS closed: back to the game's pause menu)
        g_open = false;
        g_swallowBack = true;
    }
    BYTE flags = kbm->flags;
    if (g_partyTicks > 0) { --g_partyTicks; flags |= 4; }
    KbmRaw empty = {};                   // the menu's input stays here: the character stands still
    *kbm = empty;
    kbm->flags = flags | (g_open ? 2 : 0);
    *buttons = 0;
    *ly = 0;
    *lx = 0;
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
    int voted = 0;
    for (int s = 0; s < players && s < 4; ++s) {
        g_votes[s] = !gone[s] && (flags[s] & 16);
        if (g_votes[s]) ++voted;
    }
    g_votePlayers = here;
    if (!ResultsShown()) g_rematchDone = false;
    else if (here > 1 && voted == here && !g_rematchDone && NetplayLocalSlot() == 0) {
        g_rematchDone = true;
        if (NetplayFreeRun()) NetplayRematchVoted();   // (free-running: everyone back and the next match, together)
        else g_rematchGo = true;
    }
    for (int s = 0; s < players && !g_gamePaused; ++s)   // story co-op: someone paused - everyone's game, theirs to drive
        if (!gone[s] && (flags[s] & 8)) {
            g_gamePaused = true;
            g_open = false;
            g_storyPauseTicks = 0;
            g_injectSlot = s;
            g_injectTicks = 4 * NetplayRate() / 60;
            Log(L"Pause: player %d paused the game (story co-op)", s);
        }
    // (two players at least: alone - the others gone - the online menu stays the menu; the game's own came up on top
    //  of it the moment it opened)
    if (!g_gamePaused && here > 1 && open == here) {
        g_gamePaused = true;
        g_open = false;
        g_injectSlot = first;
        g_injectTicks = 4 * NetplayRate() / 60;   // held 1/15 s
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

// FGUYMOD_PauseMenu, every frame from the HUD:
//   "open|selected|host|open count|players|quit|settings|row|top|label~value~kind;..." (the rows shown)
const char* OnlinePauseMenu() {
    if (InterlockedExchange(&g_leave, 0)) {
        NetplayLeave(L"quit to the lobby");
        InterlockedExchange(&g_quit, 1);
    }
    char buf[96];
    sprintf_s(buf, "%d|%d|%d|%d|%d|%d|%d|%d|%d|", g_open ? 1 : 0, g_sel, NetplayLocalSlot() == 0 ? 1 : 0, g_openCount,
              g_players, g_quit == 1 ? 1 : 0, g_open && g_settings ? 1 : 0, g_row, g_top);
    std::string text = buf;
    if (g_open && g_settings) {
        SettingsRow rows[32];   // (a fresh copy: the menu runs on the game's thread, this on the HUD's)
        int n = OnlineSettingsRows(rows, 32);
        for (int r = g_top; r < g_top + kVisible && r < n; ++r)
            text += std::string(rows[r].label) + "~" + rows[r].value + "~" + rows[r].kind + ";";
    }
    g_text = text;
    return g_text.c_str();
}

// FGUYMOD_PauseCursor: "x|y|show" (x, y in 1/10000 of the screen) - keyboard / mouse players get a pointer.
// (the Windows pointer instead where it can be: display.cpp DisplayOsPointer - smooth; this one moves with the HUD's
// 30 frames a second)
const char* OnlinePauseCursor() {
    bool want = g_keyboard && (g_open || BoxesShown());
    g_osPointer = DisplayOsPointer(want);
    static char buf[32];
    sprintf_s(buf, "%d|%d|%d", g_cursorX * 10000 / 32767, g_cursorY * 10000 / 32767, g_open && g_keyboard && !g_osPointer ? 1 : 0);
    return buf;
}

// FGUYMOD_DeathMouse("x0,y0,x1,y1,key;...", or "" - no death menu of mine): its buttons for the mouse.
// Returns the pointer: "x|y|show" (x, y in 1/10000 of the screen).
const char* OnlinePauseDeathMouse(const char* boxes) {
    MouseBox b[4];
    int n = ParseBoxes(boxes, b, 4);
    AcquireSRWLockExclusive(&g_boxLock);
    static int lastSig = -1;   // (logged as it changes: off, the death menu's buttons, the wheel)
    int sig = n > 0 ? 1 + b[0].kind : 0;
    if (sig != lastSig) Log(L"Pause: menu mouse %s %S", n > 0 ? (b[0].kind == 1 ? L"on (the wheel)" : L"on") : L"off", boxes ? boxes : "");
    lastSig = sig;
    memcpy(g_boxes, b, sizeof(b));
    g_boxCount = n;
    g_boxesAt = GetTickCount64();
    ReleaseSRWLockExclusive(&g_boxLock);
    static char buf[32];
    sprintf_s(buf, "%d|%d|%d", g_cursorX * 10000 / 32767, g_cursorY * 10000 / 32767,
              n > 0 && g_keyboard && !g_open && !g_osPointer ? 1 : 0);
    return buf;
}

// FGUYMOD_MenuMouse("x0,y0,x1,y1,13,2,index,chosen;..." or "", who): a menu of the game's - its items for the mouse.
// who "pause": the game's pause menu, which only the player who paused drives (anyone else's pointer would do
// nothing there); else the end of the match's menu. Returns whether the Windows pointer is shown ("1").
const char* OnlineMenuMouse(const char* boxes, const char* who) {
    MouseBox b[6];
    int n = ParseBoxes(boxes, b, 6);
    if (who && strcmp(who, "pause") == 0 && !(g_gamePaused && g_injectSlot == NetplayLocalSlot() && !g_open)) n = 0;
    if (who && strcmp(who, "results") == 0 && n > 0) g_resultsAt = GetTickCount64();
    AcquireSRWLockExclusive(&g_boxLock);
    static std::string last = "-";
    static int logged = 0;
    std::string now = n > 0 && boxes ? boxes : "";
    if (now != last && ++logged <= 60) Log(L"Pause: game menu mouse %s %S", n > 0 ? L"on" : L"off", now.c_str());
    last = now;
    memcpy(g_listBoxes, b, sizeof(b));
    g_listCount = n;
    g_listAt = GetTickCount64();
    ReleaseSRWLockExclusive(&g_boxLock);
    bool shown = DisplayOsPointer(n > 0 && g_keyboard);
    return shown ? "1" : "0";
}

// FGUYMOD_PauseSettings (the game's pause menu, its OPTIONS online): the game's options would change the settings of
// everyone's game (the menu runs on every PC alike) - the player who paused gets their own SETTINGS page instead (on
// their PC; the game stays paused meanwhile, its menu where it was). Everyone else: nothing.
void OnlinePauseSettings() {
    if (!g_gamePaused || g_injectSlot != NetplayLocalSlot()) return;
    g_open = true;
    OpenSettings();
    g_seqLen = g_seqPos = 0;   // (the click's Enter did it: the rest of it would pick OPTIONS again once I'm back)
    Log(L"Pause: my settings (from the game's pause menu)");
}

// netplay.cpp, the test bot's scripted input: it always wants a rematch (tests of the vote).
bool OnlinePauseBotVotes() { return IsBot() && ResultsShown(); }

// FGUYMOD_Rematch (the end of the match's menu): "<votes>|<players>|<my vote 0/1>|<name>:<0/1>;..." ("" offline).
const char* OnlinePauseRematch() {
    static std::string out;
    out.clear();
    if (!NetplayEnabled()) return out.c_str();
    int voted = 0;
    std::string names;
    for (int s = 0; s < 4; ++s) {
        const wchar_t* w = NetplayLobbyName(s);
        if (!w || !*w) continue;
        char n8[64];
        WideCharToMultiByte(CP_UTF8, 0, w, -1, n8, sizeof(n8), nullptr, nullptr);
        for (char* c = n8; *c; ++c) if (*c == '|' || *c == ':' || *c == ';') *c = ' ';
        names += std::string(n8) + ":" + (g_votes[s] ? "1" : "0") + ";";
        if (g_votes[s]) ++voted;
    }
    char head[32];
    sprintf_s(head, "%d|%d|%d|", voted, g_votePlayers, g_vote ? 1 : 0);
    out = head + names;
    return out.c_str();
}

// FGUYMOD_HudTakesStart: my death menu's CHANGE WEAPONS screen came up / went.
void OnlinePauseHudTakesStart(bool up) {
    if (up != g_hudTakesStart) Log(L"Pause: the weapons screen is %s (START %s)", up ? L"up" : L"gone", up ? L"leaves it" : L"pauses");
    g_hudTakesStart = up;
}

// Out of sync (netplay.cpp, the same check on every PC): leave the match - back to the lobby.
// netplay.cpp (my view turning at once): does a menu have my input now - my pause menu, the game's own pause, a HUD
// menu (death, weapons wheel...), the end of the match?
bool OnlinePauseMenusUp() { return g_open || g_gamePaused || BoxesShown() || ResultsShown(); }

bool OnlinePauseResultsShown() { return ResultsShown(); }
bool OnlinePauseTakeLeave() { return InterlockedExchange(&g_leave, 0) != 0; }

void OnlinePauseForceQuit() {
    g_open = false;
    InterlockedExchange(&g_leave, 1);   // (the HUD's next poll leaves the session and quits the level)
}

// FGUYMOD_PauseQuitDone: the HUD has quit the level.
void OnlinePauseQuitDone() { InterlockedExchange(&g_quit, 2); }
