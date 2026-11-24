// The devkit: a panel over the game, in a level - F1. The user's own ("just me"; "in game things, so for example giving unlimited
// beer to brian ... or unlimited abilities"; "a menu with the mouse"; offline and online with friends; "a private mod only on my
// PC"; then "build the rest and add a way to give other player abilities"). The code is here for everyone to read (the repository
// is public); the panel opens only on a PC that has the key file: <game>\FGUYMod\devkit.key, whose SHA-256 is kKeyHash - the file
// itself is in no release and not in the repository.
//
// A window of our own (as overlay.cpp's notices: its own thread, on top of the game while the game is in front, never taking the
// focus) - so it is the same offline and in a session. Its rows are switches, choices and buttons, worked by the mouse; while it is
// up (and while the free camera is on) the game gets no keyboard and no mouse (dinput.cpp outside sessions, kbm.cpp in them). What
// a row does is done on the game's thread (DevkitTick, from the pad poll). F1 the panel, F2 the free camera, F3 flying.
//
// Who a row is for ("For": me, the other player, everyone): god mode, unlimited ammo, unlimited items and power-ups, giving them
// all, bringing the other player here. In a session the key holder's PC says it to the others ('x': each player's switches, a
// counter of gives, a "come here" with where) and every PC does it for its copy of that player - the player's own PC for what
// is its to say (health, where it is), all of them for the counts (each PC counts its copy's rounds and items down). The other
// player's PC needs this build, no key.
//
// What the game keeps (2026-11-24, research on a test copy): a player's weapons are a table at player +0xe4, 12 bytes an entry, the
// first word a zWeapon (vtable 0xa22fa4) - both characters' weapons, their grenades, the power-ups (Ipecac 0x83, Giant Chicken
// 0x2412, Tube Man 0x80, Boom Bear 0x89, Joe 0x240e, Rupert 0x3630) and the drink (0x103): each its asset id +0x18, the count in
// hand / in the clip +0x164, the rest +0x168, the most a clip holds +0x170. An item is one with nothing in reserve and a few at
// most (a grenade's 2, a power-up's 2).

#include "common.h"

#include <bcrypt.h>
#include <algorithm>
#include <cmath>
#include <map>
#include <string>
#include <vector>

#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "bcrypt.lib")

namespace {

const char kKeyHash[] = "021ad49e1e76618beac031106e81d3e224a4855951547fcca9eed50ce8d58120";

bool g_allowed = false;
volatile LONG g_open = 0;
HWND g_panel = nullptr;

// ---- what the rows switch and ask for ----
enum Flag { kGod = 1, kAmmo = 2, kItems = 4 };
volatile LONG g_flags[4] = {};            // each player's switches (on every PC: from the key holder's word)
volatile LONG g_giveSeq[4] = {};          // gives asked for, each player (a counter: each PC gives when it grows)
LONG g_giveDone[4] = {};
volatile LONG g_bringSeq = 0, g_bringTo = -1;
float g_bringAt[3] = {};
LONG g_bringDone = 0;
bool g_heard = false;                     // (another PC's word has come: its switches are done here)
volatile LONG g_who = 0;                  // the "For" row: 0 me, 1 the other player, 2 everyone
volatile LONG g_fly = 0, g_freecam = 0, g_freeze = 0;
volatile LONG g_devFlyTest = 0;           // (the test pipe's "devkit flytest <ticks>": as W held while flying - no real keys in test copies)
enum Action { kNone = 0, kKillAll, kSavePos, kLoadPos, kGoLevel, kGive, kBring, kSpawn, kReload, kRunCommand };
volatile LONG g_action = kNone;
std::vector<std::wstring> g_levelNames;   // "FRXX"
std::vector<std::string> g_levelPaths;    // "/FGUY/Levels/Level_FRXX/FRXX" (as GAME_Portal takes it)
volatile LONG g_level = 0, g_kind = 0;
// (the level's: refreshed on the game's thread, shown on the panel's)
CRITICAL_SECTION g_lock;
std::vector<unsigned> g_kinds;            // enemy kinds seen spawned: low, high pairs
wchar_t g_note[160] = L"";                // (the last thing done, shown at the panel's foot)
std::wstring g_cmd;                       // the command line's text
bool g_typing = false;
std::string g_runCmd;
void Note(const wchar_t* text) {
    EnterCriticalSection(&g_lock);
    wcscpy_s(g_note, text);
    LeaveCriticalSection(&g_lock);
    Log(L"Devkit: %s", text);
    if (g_panel) InvalidateRect(g_panel, nullptr, FALSE);
}

unsigned char* PlayerOf(int slot);
// My player: in a session my slot's; outside sessions the one the first view shows (the story's two characters are two players -
// Brian and Stewie - and the one played is the one in view: player +0xdfc, its view's number).
int MySlot() {
    if (NetplayEnabled()) return NetplayLocalSlot();
    unsigned char* list[8];
    int n = GamePlayers(list, 8);
    for (int i = 0; i < n; ++i)
        if (*reinterpret_cast<int*>(list[i] + 0xdfc) == 0) return *reinterpret_cast<int*>(list[i] + 0xdf8);
    return 0;
}
volatile LONG g_ticks = 0;
unsigned char* PlayerOf(int slot) {
    unsigned char* list[8];
    int n = GamePlayers(list, 8);
    for (int i = 0; i < n; ++i)
        if (*reinterpret_cast<int*>(list[i] + 0xdf8) == slot) return list[i];
    return nullptr;
}
unsigned char* MyPlayer() { return PlayerOf(MySlot()); }
// A player someone plays: one with a view on the screen (player +0xdfc below the views' count, [0xb53e60]) - the story keeps a
// third player object for its own (slot 2, no view of its own to play)
bool Played(unsigned char* p) {
    int view = *reinterpret_cast<int*>(p + 0xdfc), views = *reinterpret_cast<volatile int*>(0xb53e60);
    return view >= 0 && view < (views > 0 ? views : 1);
}
// The players the "For" row means now: a mask of slots
int Targets() {
    int me = MySlot(), mask = 0;
    unsigned char* list[8];
    int n = GamePlayers(list, 8);
    for (int i = 0; i < n; ++i) {
        int s = *reinterpret_cast<int*>(list[i] + 0xdf8);
        if (s < 0 || s >= 4 || !Played(list[i])) continue;
        if (g_who == 2 || (g_who == 0 && s == me) || (g_who == 1 && s != me)) mask |= 1 << s;
    }
    if (g_who == 0 && me >= 0 && me < 4) mask |= 1 << me;   // (me, even between levels)
    return mask;
}

enum RowKind { kToggle, kButton, kChoice, kHead, kInput };
enum ChoiceOf { kChNone, kChWho, kChLevel, kChKind };
struct Row { int col; const wchar_t* label; RowKind kind; volatile LONG* value; int flag; Action action; ChoiceOf choice; };
const Row kRows[] = {
    {0, L"FOR", kHead, nullptr, 0, kNone, kChNone},
    {0, L"Who", kChoice, &g_who, 0, kNone, kChWho},
    {0, L"PLAYER", kHead, nullptr, 0, kNone, kChNone},
    {0, L"God mode", kToggle, nullptr, kGod, kNone, kChNone},
    {0, L"Unlimited ammo", kToggle, nullptr, kAmmo, kNone, kChNone},
    {0, L"Unlimited items & power-ups", kToggle, nullptr, kItems, kNone, kChNone},
    {0, L"Give all items & power-ups", kButton, nullptr, 0, kGive, kChNone},
    {0, L"Bring the other player to me", kButton, nullptr, 0, kBring, kChNone},
    {0, L"MOVE", kHead, nullptr, 0, kNone, kChNone},
    {0, L"Fly through walls  (F3)", kToggle, &g_fly, 0, kNone, kChNone},
    {0, L"Free camera  (F2)", kToggle, &g_freecam, 0, kNone, kChNone},
    {0, L"Save my position", kButton, nullptr, 0, kSavePos, kChNone},
    {0, L"Go back to the saved position", kButton, nullptr, 0, kLoadPos, kChNone},
    {1, L"ENEMIES", kHead, nullptr, 0, kNone, kChNone},
    {1, L"Kill all enemies", kButton, nullptr, 0, kKillAll, kChNone},
    {1, L"Freeze enemies", kToggle, &g_freeze, 0, kNone, kChNone},
    {1, L"Enemy", kChoice, &g_kind, 0, kNone, kChKind},
    {1, L"Spawn one in front of me", kButton, nullptr, 0, kSpawn, kChNone},
    {1, L"LEVEL", kHead, nullptr, 0, kNone, kChNone},
    {1, L"Level", kChoice, &g_level, 0, kNone, kChLevel},
    {1, L"Go to that level", kButton, nullptr, 0, kGoLevel, kChNone},
    {1, L"MODS", kHead, nullptr, 0, kNone, kChNone},
    {1, L"Reload skins, emotes & mods", kButton, nullptr, 0, kReload, kChNone},
    {1, L"COMMAND", kHead, nullptr, 0, kNone, kChNone},
    {1, L"", kInput, nullptr, 0, kNone, kChNone},
};
const int kRowCount = (int)(sizeof(kRows) / sizeof(kRows[0]));
// (sizes for a window 1080 high: scaled to the game's window - at 4K a panel of 430 dots is a stamp)
float g_scale = 1;
int S(int v) { return (int)(v * g_scale + 0.5f); }
#define kRowH S(34)
#define kHeadH S(28)
#define kTitleH S(52)
#define kFootH S(40)
#define kColW S(420)
#define kGap S(16)
#define kWidth (kColW * 2 + kGap)
#define kPad S(14)
void ScaleTo(const RECT& game) {
    float s = (game.bottom - game.top) / 1080.0f;
    g_scale = s < 1 ? 1 : s > 3 ? 3 : s;
}
int g_hover = -1;
bool g_forced = false;   // (the test pipe's "devkit show": up off the screen, whatever is in front - for a picture of it)

int RowTop(int row) {
    int y = kTitleH;
    for (int i = 0; i < row; ++i)
        if (kRows[i].col == kRows[row].col) y += kRows[i].kind == kHead ? kHeadH : kRowH;
    return y;
}
int RowLeft(int row) { return kRows[row].col * (kColW + kGap); }
int PanelHeight() {
    int h = 0;
    for (int i = 0; i < kRowCount; ++i) { int b = RowTop(i) + (kRows[i].kind == kHead ? kHeadH : kRowH); if (b > h) h = b; }
    return h + kFootH;
}
int RowAt(int x, int y) {
    for (int i = 0; i < kRowCount; ++i) {
        int top = RowTop(i), left = RowLeft(i), h = kRows[i].kind == kHead ? kHeadH : kRowH;
        if (x >= left && x < left + kColW && y >= top && y < top + h) return kRows[i].kind == kHead ? -1 : i;
    }
    return -1;
}

// A switch for the "For" row's players: on if it is on for all of them
bool FlagOn(int flag) {
    int mask = Targets();
    if (!mask) return false;
    for (int s = 0; s < 4; ++s)
        if ((mask >> s & 1) && !(g_flags[s] & flag)) return false;
    return true;
}
std::wstring ChoiceText(ChoiceOf c) {
    wchar_t t[64] = L"-";
    EnterCriticalSection(&g_lock);
    if (c == kChWho) {
        static const wchar_t* who[3] = {L"Me", L"Other player", L"Everyone"};
        wcscpy_s(t, who[g_who >= 0 && g_who < 3 ? g_who : 0]);
    } else if (c == kChLevel) {
        if (g_level >= 0 && g_level < (LONG)g_levelNames.size()) wcscpy_s(t, g_levelNames[g_level].c_str());
    } else if (c == kChKind) {
        int n = (int)g_kinds.size() / 2;
        if (n == 0) wcscpy_s(t, L"none seen yet");
        else { int k = g_kind % n; swprintf_s(t, L"%d/%d  (%x:%x)", k + 1, n, g_kinds[k * 2], g_kinds[k * 2 + 1]); }
    }
    LeaveCriticalSection(&g_lock);
    return t;
}
int ChoiceCount(ChoiceOf c) {
    EnterCriticalSection(&g_lock);
    int n = c == kChWho ? 3 : c == kChLevel ? (int)g_levelNames.size() : c == kChKind ? (int)g_kinds.size() / 2 : 0;
    LeaveCriticalSection(&g_lock);
    return n;
}

void Draw(HWND wnd, HDC dc) {
    RECT all;
    GetClientRect(wnd, &all);
    HDC mem = CreateCompatibleDC(dc);
    HBITMAP bmp = CreateCompatibleBitmap(dc, all.right, all.bottom);
    HGDIOBJ oldBmp = SelectObject(mem, bmp);
    const COLORREF navy = RGB(0x13, 0x21, 0x3C), yellow = RGB(0xF5, 0xD3, 0x3A), white = RGB(255, 255, 255),
                   dim = RGB(0x9A, 0xA7, 0xC0), rowHot = RGB(0x24, 0x3A, 0x66), on = RGB(0x4C, 0xD9, 0x64), field = RGB(0x0B, 0x15, 0x29);
    HBRUSH bg = CreateSolidBrush(navy);
    FillRect(mem, &all, bg);
    DeleteObject(bg);
    HBRUSH edge = CreateSolidBrush(yellow);
    FrameRect(mem, &all, edge);
    HFONT title = CreateFontW(-S(24), 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                              CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
    HFONT text = CreateFontW(-S(17), 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                             CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
    HFONT small = CreateFontW(-S(14), 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                              CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
    SetBkMode(mem, TRANSPARENT);
    HGDIOBJ oldFont = SelectObject(mem, title);
    SetTextColor(mem, yellow);
    RECT r = {kPad, 0, all.right - kPad, kTitleH};
    DrawTextW(mem, L"DEVKIT", -1, &r, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    SelectObject(mem, small);
    SetTextColor(mem, dim);
    DrawTextW(mem, L"F1 panel   F2 free camera   F3 fly   Esc closes", -1, &r, DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
    for (int i = 0; i < kRowCount; ++i) {
        const Row& row = kRows[i];
        int top = RowTop(i), left = RowLeft(i);
        if (row.kind == kHead) {
            SelectObject(mem, small);
            SetTextColor(mem, yellow);
            RECT h = {left + kPad, top, left + kColW - kPad, top + kHeadH};
            DrawTextW(mem, row.label, -1, &h, DT_LEFT | DT_BOTTOM | DT_SINGLELINE);
            continue;
        }
        RECT box = {left + S(6), top + S(2), left + kColW - S(6), top + kRowH - S(2)};
        if (row.kind == kInput) {
            HBRUSH f = CreateSolidBrush(field);
            FillRect(mem, &box, f);
            DeleteObject(f);
            FrameRect(mem, &box, edge);
        } else if (i == g_hover) {
            HBRUSH hot = CreateSolidBrush(rowHot);
            FillRect(mem, &box, hot);
            DeleteObject(hot);
        }
        SelectObject(mem, text);
        RECT l = {left + kPad + S(6), top, left + kColW - kPad, top + kRowH};
        if (row.kind == kInput) {
            EnterCriticalSection(&g_lock);
            std::wstring shown = g_typing ? L"> " + g_cmd + L"_" : g_cmd.empty() ? L"Click here, type a command, Enter" : L"> " + g_cmd;
            LeaveCriticalSection(&g_lock);
            SetTextColor(mem, g_typing || !g_cmd.empty() ? white : dim);
            DrawTextW(mem, shown.c_str(), -1, &l, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);
            continue;
        }
        SetTextColor(mem, white);
        DrawTextW(mem, row.label, -1, &l, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
        if (row.kind == kToggle) {
            bool is = row.flag ? FlagOn(row.flag) : *row.value != 0;
            SetTextColor(mem, is ? on : dim);
            DrawTextW(mem, is ? L"ON" : L"OFF", -1, &l, DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
        } else if (row.kind == kChoice) {
            std::wstring name = L"\x25C4  " + ChoiceText(row.choice) + L"  \x25BA";
            SetTextColor(mem, yellow);
            DrawTextW(mem, name.c_str(), -1, &l, DT_RIGHT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
        } else {
            SetTextColor(mem, dim);
            DrawTextW(mem, L"\x25BA", -1, &l, DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
        }
    }
    wchar_t note[160];
    EnterCriticalSection(&g_lock);
    wcscpy_s(note, g_note);
    LeaveCriticalSection(&g_lock);
    SelectObject(mem, small);
    SetTextColor(mem, dim);
    RECT f = {kPad, all.bottom - kFootH, all.right - kPad, all.bottom};
    DrawTextW(mem, note, -1, &f, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);
    SelectObject(mem, oldFont);
    DeleteObject(title);
    DeleteObject(text);
    DeleteObject(small);
    DeleteObject(edge);
    BitBlt(dc, 0, 0, all.right, all.bottom, mem, 0, 0, SRCCOPY);
    SelectObject(mem, oldBmp);
    DeleteObject(bmp);
    DeleteDC(mem);
}
void Paint(HWND wnd) {
    PAINTSTRUCT ps;
    HDC dc = BeginPaint(wnd, &ps);
    Draw(wnd, dc);
    EndPaint(wnd, &ps);
}

// ---- typing the command line: a keyboard hook of this thread while it is being typed in (the panel never has the focus) ----
HHOOK g_kbHook = nullptr;
ULONGLONG g_typingEnded = 0;
void StopTyping() {
    g_typing = false;
    g_typingEnded = GetTickCount64();
    if (g_kbHook) { UnhookWindowsHookEx(g_kbHook); g_kbHook = nullptr; }
    if (g_panel) InvalidateRect(g_panel, nullptr, FALSE);
}
LRESULT CALLBACK TypeHook(int code, WPARAM wp, LPARAM lp) {
    if (code != HC_ACTION || !g_typing) return CallNextHookEx(nullptr, code, wp, lp);
    auto k = reinterpret_cast<KBDLLHOOKSTRUCT*>(lp);
    if (wp == WM_KEYDOWN || wp == WM_SYSKEYDOWN) {
        DWORD vk = k->vkCode;
        if (vk == VK_RETURN) {
            EnterCriticalSection(&g_lock);
            char line[512];
            WideCharToMultiByte(CP_UTF8, 0, g_cmd.c_str(), -1, line, sizeof(line), nullptr, nullptr);
            g_runCmd = line;
            LeaveCriticalSection(&g_lock);
            InterlockedExchange(&g_action, kRunCommand);
            StopTyping();
        } else if (vk == VK_ESCAPE || vk == VK_F1) {
            StopTyping();
        } else if (vk == VK_BACK) {
            EnterCriticalSection(&g_lock);
            if (!g_cmd.empty()) g_cmd.pop_back();
            LeaveCriticalSection(&g_lock);
        } else {
            BYTE state[256] = {};
            if (GetAsyncKeyState(VK_SHIFT) & 0x8000) state[VK_SHIFT] = 0x80;
            if (GetKeyState(VK_CAPITAL) & 1) state[VK_CAPITAL] = 1;
            wchar_t ch[4] = {};
            int n = ToUnicodeEx(vk, k->scanCode, state, ch, 4, 0, GetKeyboardLayout(0));
            if (n == 1 && ch[0] >= 32) {
                EnterCriticalSection(&g_lock);
                if (g_cmd.size() < 200) g_cmd += ch[0];
                LeaveCriticalSection(&g_lock);
            }
        }
        if (g_panel) InvalidateRect(g_panel, nullptr, FALSE);
    }
    return 1;   // (none of it the game's)
}
void StartTyping() {
    if (g_typing) return;
    g_typing = true;
    EnterCriticalSection(&g_lock);
    g_cmd.clear();
    LeaveCriticalSection(&g_lock);
    g_kbHook = SetWindowsHookExW(WH_KEYBOARD_LL, TypeHook, GetModuleHandleW(nullptr), 0);
}

void Click(int row, int x) {
    if (row < 0 || row >= kRowCount) return;
    const Row& r = kRows[row];
    if (r.kind == kInput) { StartTyping(); return; }
    if (g_typing) StopTyping();
    if (r.kind == kToggle && r.flag) {
        bool on = !FlagOn(r.flag);
        int mask = Targets();
        for (int s = 0; s < 4; ++s)
            if (mask >> s & 1) {
                LONG v = g_flags[s];
                InterlockedExchange(&g_flags[s], on ? (v | r.flag) : (v & ~r.flag));
            }
        Log(L"Devkit: %s %s (players %x)", r.label, on ? L"on" : L"off", mask);
    } else if (r.kind == kToggle) {
        InterlockedExchange(r.value, *r.value ? 0 : 1);
        Log(L"Devkit: %s %s", r.label, *r.value ? L"on" : L"off");
    } else if (r.kind == kChoice) {
        LONG n = ChoiceCount(r.choice);
        int left = RowLeft(row);
        if (n > 0) InterlockedExchange(r.value, ((*r.value % n) + (x - left < kColW * 3 / 4 ? n - 1 : 1)) % n);
    } else if (r.kind == kButton) {
        InterlockedExchange(&g_action, r.action);
    }
}

bool GameInFront(RECT* game) {
    HWND gameWnd = DisplayMainWindow();
    return gameWnd && IsWindowVisible(gameWnd) && !IsIconic(gameWnd) && GetForegroundWindow() == gameWnd && DisplayWindowRect(game);
}

void Show(bool open) {
    InterlockedExchange(&g_open, open ? 1 : 0);
    RECT game;
    if (open && GameInFront(&game)) {
        ScaleTo(game);
        int h = PanelHeight();
        int x = game.left + S(30), y = game.top + ((game.bottom - game.top) - h) / 3;
        SetWindowPos(g_panel, HWND_TOPMOST, x, y, kWidth, h, SWP_NOACTIVATE | SWP_SHOWWINDOW);
        SetCursorPos(x + kColW / 2, y + RowTop(1) + kRowH / 2);   // (the game hides the pointer over its own window: onto the panel)
        InvalidateRect(g_panel, nullptr, FALSE);
    } else {
        InterlockedExchange(&g_open, 0);
        if (g_typing) StopTyping();
        ShowWindow(g_panel, SW_HIDE);
    }
    Log(L"Devkit: the panel %s", g_open ? L"opens" : L"closes");
}

LRESULT CALLBACK PanelProc(HWND wnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_PAINT: Paint(wnd); return 0;
    case WM_PRINTCLIENT: Draw(wnd, reinterpret_cast<HDC>(wp)); return 0;   // (PrintWindow: the test's picture of it)
    case WM_ERASEBKGND: return 1;
    case WM_MOUSEACTIVATE: return MA_NOACTIVATE;
    case WM_SETCURSOR: SetCursor(LoadCursorW(nullptr, IDC_ARROW)); return TRUE;
    case WM_MOUSEMOVE: {
        int row = RowAt((short)LOWORD(lp), (short)HIWORD(lp));
        if (row != g_hover) { g_hover = row; InvalidateRect(wnd, nullptr, FALSE); }
        return 0;
    }
    case WM_LBUTTONDOWN:
        Click(RowAt((short)LOWORD(lp), (short)HIWORD(lp)), (short)LOWORD(lp));
        InvalidateRect(wnd, nullptr, FALSE);
        return 0;
    case WM_APP + 1:
        g_forced = wp != 0;
        { RECT game; if (DisplayWindowRect(&game)) ScaleTo(game); }
        if (g_forced) SetWindowPos(wnd, HWND_TOPMOST, -3000, 100, kWidth, PanelHeight(), SWP_NOACTIVATE | SWP_SHOWWINDOW);
        else ShowWindow(wnd, SW_HIDE);
        g_hover = (int)lp;
        InvalidateRect(wnd, nullptr, FALSE);
        return 0;
    case WM_TIMER: {
        if (g_forced) return 0;
        // F1 the panel, F2 the free camera, F3 flying, Esc closes the panel - with the game in front (the keys' state: this window
        // never has the focus). Not while the command line is typed in (its keys are its own).
        static bool was[4] = {};
        RECT game;
        bool front = GameInFront(&game), typing = g_typing || GetTickCount64() - g_typingEnded < 300;
        const int keys[4] = {VK_F1, VK_ESCAPE, VK_F2, VK_F3};
        bool down[4];
        for (int k = 0; k < 4; ++k) down[k] = front && !typing && (GetAsyncKeyState(keys[k]) & 0x8000) != 0;
        if (down[0] && !was[0]) Show(!g_open);
        else if (down[1] && !was[1] && g_open) Show(false);
        else if (g_open && !front) Show(false);   // (another program in front, the game hidden)
        if (down[2] && !was[2]) { InterlockedExchange(&g_freecam, g_freecam ? 0 : 1); InvalidateRect(wnd, nullptr, FALSE); }
        if (down[3] && !was[3]) { InterlockedExchange(&g_fly, g_fly ? 0 : 1); InvalidateRect(wnd, nullptr, FALSE); }
        for (int k = 0; k < 4; ++k) was[k] = down[k];
        if (g_open) DisplayOsPointer(true);   // (the Windows pointer over the game's own window too, while the panel is up)
        static ULONGLONG lastPaint = 0;   // (the switches can change from elsewhere: the other PC's word, the hot keys)
        if (g_open && GetTickCount64() - lastPaint > 500) { lastPaint = GetTickCount64(); InvalidateRect(wnd, nullptr, FALSE); }
        return 0;
    }
    }
    return DefWindowProcW(wnd, msg, wp, lp);
}

DWORD WINAPI PanelThread(LPVOID) {
    WNDCLASSW wc{};
    wc.lpfnWndProc = PanelProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"FGUYModDevkit";
    RegisterClassW(&wc);
    g_panel = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, wc.lpszClassName, L"", WS_POPUP, 0, 0, 856,
                              640, nullptr, nullptr, wc.hInstance, nullptr);
    if (!g_panel) return 0;
    SetTimer(g_panel, 1, 40, nullptr);
    MSG m;
    while (GetMessageW(&m, nullptr, 0, 0) > 0) {
        TranslateMessage(&m);
        DispatchMessageW(&m);
    }
    return 0;
}

std::string Sha256Of(const std::vector<unsigned char>& data) {
    BCRYPT_ALG_HANDLE alg = nullptr;
    BCRYPT_HASH_HANDLE h = nullptr;
    unsigned char digest[32];
    std::string hex;
    if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0) == 0 && BCryptCreateHash(alg, &h, nullptr, 0, nullptr, 0, 0) == 0 &&
        BCryptHashData(h, const_cast<unsigned char*>(data.data()), (ULONG)data.size(), 0) == 0 &&
        BCryptFinishHash(h, digest, sizeof(digest), 0) == 0) {
        static const char* d = "0123456789abcdef";
        for (unsigned char c : digest) { hex += d[c >> 4]; hex += d[c & 15]; }
    }
    if (h) BCryptDestroyHash(h);
    if (alg) BCryptCloseAlgorithmProvider(alg, 0);
    return hex;
}

bool KeyFileHere() {
    wchar_t path[MAX_PATH];
    swprintf_s(path, L"%s\\devkit.key", g_modDir);
    HANDLE f = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    std::vector<unsigned char> data(512);
    DWORD got = 0;
    ReadFile(f, data.data(), (DWORD)data.size(), &got, nullptr);
    CloseHandle(f);
    while (got > 0 && (data[got - 1] == '\r' || data[got - 1] == '\n' || data[got - 1] == ' ')) --got;
    data.resize(got);
    return got > 0 && Sha256Of(data) == kKeyHash;
}

// The story's levels this game has: <game>\FGUY\Levels\<name>.ho and ...\Level_<name>\<name>.ho, names of four ending in "XX"
// (the other endings are the multiplayer modes' maps). The path GAME_Portal takes: from "/FGUY/", without ".ho".
void FindLevels() {
    wchar_t dir[MAX_PATH];
    swprintf_s(dir, L"%s\\FGUY\\Levels", g_gameDir);
    auto add = [](const wchar_t* file, const wchar_t* sub) {
        if (wcslen(file) != 7 || _wcsicmp(file + 4, L".ho") != 0 || _wcsnicmp(file + 2, L"XX", 2) != 0) return;
        std::wstring name(file, 4);
        for (const std::wstring& have : g_levelNames) if (_wcsicmp(have.c_str(), name.c_str()) == 0) return;
        char path[96];
        if (sub) sprintf_s(path, "/FGUY/Levels/%S/%S", sub, name.c_str());
        else sprintf_s(path, "/FGUY/Levels/%S", name.c_str());
        g_levelNames.push_back(name);
        g_levelPaths.push_back(path);
    };
    WIN32_FIND_DATAW fd;
    wchar_t pat[MAX_PATH];
    swprintf_s(pat, L"%s\\*", dir);
    HANDLE h = FindFirstFileW(pat, &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            if (fd.cFileName[0] == L'.') continue;
            wchar_t inner[MAX_PATH];
            swprintf_s(inner, L"%s\\%s\\*.ho", dir, fd.cFileName);
            WIN32_FIND_DATAW in;
            HANDLE hi = FindFirstFileW(inner, &in);
            if (hi == INVALID_HANDLE_VALUE) continue;
            do add(in.cFileName, fd.cFileName); while (FindNextFileW(hi, &in));
            FindClose(hi);
        } else {
            add(fd.cFileName, nullptr);
        }
    } while (FindNextFileW(h, &fd));
    FindClose(h);
}

// ---- the game's thread ----
float* HealthOf(unsigned char* player) {
    auto h = *reinterpret_cast<unsigned char**>(player + 0x105c);
    return h && !IsBadWritePtr(h + 0x50, 4) ? reinterpret_cast<float*>(h + 0x50) : nullptr;
}
// (the player's transform [+0x38] +0x30, and its collision shape's place [[+0x104c] +0x60] +0xe0: both, as the research "teleport")
bool PlayerPlace(unsigned char* player, float** feet, float** shape) {
    auto transform = *reinterpret_cast<unsigned char**>(player + 0x38);
    auto proxy = *reinterpret_cast<unsigned char**>(player + 0x104c);
    auto phantom = proxy && !IsBadReadPtr(proxy + 0x60, 4) ? *reinterpret_cast<unsigned char**>(proxy + 0x60) : nullptr;
    if (!transform || !phantom || IsBadWritePtr(transform + 0x30, 12) || IsBadWritePtr(phantom + 0xe0, 12)) return false;
    *feet = reinterpret_cast<float*>(transform + 0x30);
    *shape = reinterpret_cast<float*>(phantom + 0xe0);
    return true;
}
bool PutPlayer(unsigned char* player, const float* at) {
    float *feet, *shape;
    if (!player || !PlayerPlace(player, &feet, &shape)) return false;
    float lift = shape[1] - feet[1];   // (the shape sits a little above the feet)
    shape[0] = at[0]; shape[1] = at[1] + lift; shape[2] = at[2];
    feet[0] = at[0]; feet[1] = at[1]; feet[2] = at[2];
    return true;
}
float CamYaw(unsigned char* player) {   // (the player's camera's yaw, radians: firstperson.cpp's kCamYaw)
    return IsBadReadPtr(player + 0xf3b4, 4) ? 0 : *reinterpret_cast<float*>(player + 0xf3b4);
}

// The player's weapons (above): each zWeapon it points at from +0xe0 to +0x300 - there are a few tables there (each character's
// weapons, the power-ups), entries 12 bytes apart, the tables not on one grid: every word looked at, each weapon once.
const uintptr_t kWeaponVtable = 0xa22fa4;
template <class F> void ForWeapons(unsigned char* player, F fn) {
    unsigned char* seen[64];
    int n = 0;
    for (int off = 0xe0; off < 0x300 && n < 64; off += 4) {
        auto w = *reinterpret_cast<unsigned char**>(player + off);
        if ((uintptr_t)w < 0x10000 || IsBadWritePtr(w, 0x174) || *reinterpret_cast<uintptr_t*>(w) != kWeaponVtable) continue;
        bool dup = false;
        for (int i = 0; i < n && !dup; ++i) dup = seen[i] == w;
        if (dup) continue;
        seen[n++] = w;
        fn(w);
    }
}
bool IsItem(unsigned char* w) {
    return *reinterpret_cast<int*>(w + 0x168) == 0 && *reinterpret_cast<int*>(w + 0x170) >= 1 && *reinterpret_cast<int*>(w + 0x170) <= 9;
}
std::map<unsigned char*, int> g_reserveTop;   // (each weapon's most in reserve seen: held there with unlimited ammo)
// Holds a player's rounds / items at the most (ammo: the clip full and the reserve at its most seen; items: at least what a clip of
// them holds). `give`: the items once.
volatile LONG g_fills = 0, g_fillWrites = 0;
void Fill(unsigned char* player, bool ammo, bool items) {
    InterlockedIncrement(&g_fills);
    ForWeapons(player, [&](unsigned char* w) {
        int* clip = reinterpret_cast<int*>(w + 0x164);
        int* rest = reinterpret_cast<int*>(w + 0x168);
        int size = *reinterpret_cast<int*>(w + 0x170);
        int& top = g_reserveTop[w];
        if (*rest > top) top = *rest;
        if (IsItem(w)) {
            if (items && *clip < size) { *clip = size; InterlockedIncrement(&g_fillWrites); }
        } else if (ammo) {
            if (size > 0 && *clip < size) { *clip = size; InterlockedIncrement(&g_fillWrites); }
            if (*rest < top) *rest = top;
        }
    });
}

// The devkit's word between PCs ('x')
#pragma pack(push, 1)
struct PktDevkit {
    char type;
    BYTE slot;
    BYTE flags[4];
    unsigned give[4];
    unsigned bringSeq;
    signed char bringTo;
    float bringAt[3];
};
#pragma pack(pop)
void SendWord() {
    PktDevkit p{};
    p.type = 'x';
    p.slot = (BYTE)MySlot();
    for (int s = 0; s < 4; ++s) { p.flags[s] = (BYTE)g_flags[s]; p.give[s] = (unsigned)g_giveSeq[s]; }
    p.bringSeq = (unsigned)g_bringSeq;
    p.bringTo = (signed char)g_bringTo;
    memcpy(p.bringAt, g_bringAt, sizeof(p.bringAt));
    NetplayDevSend(&p, sizeof(p));
}

// God mode outside sessions: the players' TakeDamage (vtable slot 58 - netplay.cpp's in sessions) says no to a player whose god mode is on
using TakeDamage_t = char(__thiscall*)(void* self, void* info);
TakeDamage_t g_realTakeDamage = nullptr;
char __fastcall Devkit_TakeDamage(void* self, void*, void* info) {
    int s = self && !IsBadReadPtr(static_cast<unsigned char*>(self) + 0xdf8, 4) ? *reinterpret_cast<int*>(static_cast<unsigned char*>(self) + 0xdf8) : -1;
    if (s >= 0 && s < 4 && (g_flags[s] & kGod)) return 0;
    return g_realTakeDamage(self, info);
}

// ---- the free camera: the view drawn from where the camera is flown to (splitview.cpp hands us the drawn camera; only drawing) ----
SRWLOCK g_camLock = SRWLOCK_INIT;
struct FreeCam { bool have = false; float pos[3] = {}, yaw = 0, pitch = 0, signR = 1, signU = 1; LARGE_INTEGER last{}; } g_cam;
void Cross(const float* a, const float* b, float* o) {
    o[0] = a[1] * b[2] - a[2] * b[1];
    o[1] = a[2] * b[0] - a[0] * b[2];
    o[2] = a[0] * b[1] - a[1] * b[0];
}
float Dot(const float* a, const float* b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
void CamAxes(float yaw, float pitch, float signR, float signU, float* f, float* r, float* u) {
    f[0] = cosf(pitch) * sinf(yaw); f[1] = sinf(pitch); f[2] = cosf(pitch) * cosf(yaw);
    r[0] = cosf(yaw) * signR; r[1] = 0; r[2] = -sinf(yaw) * signR;
    float c[3];
    Cross(f, r, c);
    for (int k = 0; k < 3; ++k) u[k] = c[k] * signU;
}
bool Key(int vk) { return (GetAsyncKeyState(vk) & 0x8000) != 0; }

}  // namespace

bool DevkitOpen() { return g_open != 0; }
bool DevkitBlocksInput() { return g_open != 0 || g_freecam != 0; }

void DevkitDrawnCamera(int view, float* m) {
    (void)view;
    if (!g_freecam) {
        if (g_cam.have) { AcquireSRWLockExclusive(&g_camLock); g_cam.have = false; ReleaseSRWLockExclusive(&g_camLock); }
        return;
    }
    AcquireSRWLockExclusive(&g_camLock);
    FreeCam& c = g_cam;
    LARGE_INTEGER now, freq;
    QueryPerformanceCounter(&now);
    QueryPerformanceFrequency(&freq);
    if (!c.have) {   // (from where the game's camera is now)
        c.pos[0] = m[3]; c.pos[1] = m[7]; c.pos[2] = m[11];
        float f0[3] = {m[2], m[6], m[10]}, r0[3] = {m[0], m[4], m[8]}, u0[3] = {m[1], m[5], m[9]};
        c.yaw = atan2f(f0[0], f0[2]);
        c.pitch = asinf(f0[1] < -1 ? -1 : f0[1] > 1 ? 1 : f0[1]);
        float rr[3] = {cosf(c.yaw), 0, -sinf(c.yaw)};
        c.signR = Dot(r0, rr) < 0 ? -1.f : 1.f;
        float f[3], r[3], u[3];
        CamAxes(c.yaw, c.pitch, c.signR, 1, f, r, u);
        c.signU = Dot(u0, u) < 0 ? -1.f : 1.f;
        c.have = true;
        c.last = now;
    }
    float dt = (float)(now.QuadPart - c.last.QuadPart) / (float)freq.QuadPart;
    if (dt > 0.001f) {   // (once a frame: the view's two cameras come one after the other)
        c.last = now;
        if (dt > 0.1f) dt = 0.1f;
        RECT game;
        HWND wnd = DisplayMainWindow();
        bool front = wnd && GetForegroundWindow() == wnd && DisplayWindowRect(&game);
        if (front && !g_open) {
            POINT mid = {(game.left + game.right) / 2, (game.top + game.bottom) / 2}, at;
            GetCursorPos(&at);
            static bool centred = false;
            if (centred) {
                c.yaw += (at.x - mid.x) * 0.003f * c.signR;
                c.pitch -= (at.y - mid.y) * 0.003f;
                if (c.pitch > 1.5f) c.pitch = 1.5f;
                if (c.pitch < -1.5f) c.pitch = -1.5f;
            }
            SetCursorPos(mid.x, mid.y);
            centred = true;
            float f[3], r[3], u[3];
            CamAxes(c.yaw, c.pitch, c.signR, c.signU, f, r, u);
            float speed = (Key(VK_SHIFT) ? 30.f : 8.f) * dt, mv[3] = {};
            for (int k = 0; k < 3; ++k) {
                if (Key('W')) mv[k] += f[k];
                if (Key('S')) mv[k] -= f[k];
                if (Key('D')) mv[k] += r[k];
                if (Key('A')) mv[k] -= r[k];
            }
            if (Key(VK_SPACE)) mv[1] += 1;
            if (Key(VK_CONTROL) || Key('C')) mv[1] -= 1;
            for (int k = 0; k < 3; ++k) c.pos[k] += mv[k] * speed;
        }
    }
    float f[3], r[3], u[3];
    CamAxes(c.yaw, c.pitch, c.signR, c.signU, f, r, u);
    m[0] = r[0]; m[4] = r[1]; m[8] = r[2];
    m[1] = u[0]; m[5] = u[1]; m[9] = u[2];
    m[2] = f[0]; m[6] = f[1]; m[10] = f[2];
    m[3] = c.pos[0]; m[7] = c.pos[1]; m[11] = c.pos[2];
    ReleaseSRWLockExclusive(&g_camLock);
}

// The key holder's word from another PC: everyone's switches, gives, a "come here"
void DevkitPacketIn(int slot, const char* buf, int n) {
    if (n < (int)sizeof(PktDevkit)) return;
    PktDevkit p;
    memcpy(&p, buf, sizeof(p));
    if (g_allowed && slot != MySlot()) return;   // (the key holder's own switches are its own - it listens to no one)
    static bool told = false;
    if (!told) { told = true; Log(L"Devkit: player %d's devkit is on in this session - its switches are done here too", slot); }
    for (int s = 0; s < 4; ++s) {
        InterlockedExchange(&g_flags[s], p.flags[s]);
        InterlockedExchange(&g_giveSeq[s], (LONG)p.give[s]);
    }
    if ((LONG)p.bringSeq != g_bringSeq) {
        memcpy(g_bringAt, p.bringAt, sizeof(g_bringAt));
        InterlockedExchange(&g_bringTo, p.bringTo);
        InterlockedExchange(&g_bringSeq, (LONG)p.bringSeq);
    }
    g_heard = true;
}

void DevkitTest(const char* what, int value);

void DevkitInit() {
    InitializeCriticalSection(&g_lock);
    g_allowed = KeyFileHere();
    if (!g_allowed) return;
    FindLevels();
    Log(L"Devkit: this PC has the key - F1 in a level opens it (%d story levels found)", (int)g_levelNames.size());
    if (!NetplayEnabled()) {   // (outside sessions: god mode by the players' TakeDamage; the enemies' spawning and damage watched)
        auto slot = reinterpret_cast<void**>(0xa1af14) + 58;
        DWORD old;
        VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &old);
        g_realTakeDamage = reinterpret_cast<TakeDamage_t>(*slot);
        *slot = reinterpret_cast<void*>(&Devkit_TakeDamage);
        VirtualProtect(slot, sizeof(void*), old, &old);
        NetplayDevOffline();
    }
    CreateThread(nullptr, 0, PanelThread, nullptr, 0, nullptr);
}

void DevkitTick() {
    InterlockedIncrement(&g_ticks);
    static int noKeySlot = -2;   // (test copies share one folder, one key: FGUYMOD_TEST_DEVKIT_NOKEY=<slot> - that seat's copy has none)
    if (noKeySlot == -2) noKeySlot = SettingInt(L"Debug", L"_", L"FGUYMOD_TEST_DEVKIT_NOKEY", -1);
    if (g_allowed && noKeySlot >= 0 && NetplayEnabled() && NetplayLocalSlot() == noKeySlot) {
        g_allowed = false;
        Log(L"Devkit: test - this seat has no key (FGUYMOD_TEST_DEVKIT_NOKEY)");
    }
    if (!g_allowed && !g_heard) return;
    // (the pad poll comes thousands of times a second outside sessions: what follows at most every 4 ms)
    static LARGE_INTEGER lastRun{}, qpf{};
    LARGE_INTEGER nowQ;
    QueryPerformanceCounter(&nowQ);
    if (!qpf.QuadPart) QueryPerformanceFrequency(&qpf);
    if (lastRun.QuadPart && (nowQ.QuadPart - lastRun.QuadPart) * 1000 < qpf.QuadPart * 4) return;
    lastRun = nowQ;
    int me = MySlot();
    ULONGLONG now = GetTickCount64();
    // Everyone's switches - on every PC, for its copy of each player
    static bool godWas = false;
    bool myGod = me >= 0 && me < 4 && (g_flags[me] & kGod);
    if (myGod != godWas) { godWas = myGod; NetplayTestGod(myGod); }   // (in sessions: my player's hits not taken - netplay.cpp)
    static float top[4] = {};
    for (int s = 0; s < 4; ++s) {
        unsigned char* p = PlayerOf(s);
        if (!p) { top[s] = 0; continue; }
        LONG f = g_flags[s];
        if (float* hp = HealthOf(p)) {
            if (*hp > top[s]) top[s] = *hp;
            if ((f & kGod) && *hp > 0 && *hp < top[s]) *hp = top[s];
        }
        if (f & (kAmmo | kItems)) Fill(p, (f & kAmmo) != 0, (f & kItems) != 0);
        if (g_giveSeq[s] != g_giveDone[s]) {
            g_giveDone[s] = g_giveSeq[s];
            Fill(p, false, true);
            Log(L"Devkit: player %d given every item and power-up", s);
        }
    }
    if (g_bringSeq != g_bringDone) {   // ("come here": the player's own PC puts it there)
        g_bringDone = g_bringSeq;
        if (g_bringTo >= 0 && g_bringTo < 4 && (!NetplayEnabled() || g_bringTo == me)) {
            float at[3] = {g_bringAt[0] + 1.5f, g_bringAt[1] + 0.3f, g_bringAt[2]};
            bool ok = PutPlayer(PlayerOf(g_bringTo), at);
            Log(L"Devkit: player %d brought to (%.0f %.0f %.0f)%s", (int)g_bringTo, at[0], at[1], at[2], ok ? L"" : L" - NOT (no player)");
        }
    }
    if (!g_allowed) return;
    // My word to the others: when it changes, and once a second while anything is on
    static BYTE sentFlags[4] = {};
    static LONG sentGive[4] = {}, sentBring = 0;
    static ULONGLONG sentAt = 0;
    bool changed = sentBring != g_bringSeq, any = false;
    for (int s = 0; s < 4; ++s) {
        changed |= sentFlags[s] != (BYTE)g_flags[s] || sentGive[s] != g_giveSeq[s];
        any |= g_flags[s] != 0;
    }
    if (NetplayEnabled() && (changed || ((any || now - sentAt < 5000) && now - sentAt > 1000))) {
        if (changed) sentAt = now;
        for (int s = 0; s < 4; ++s) { sentFlags[s] = (BYTE)g_flags[s]; sentGive[s] = g_giveSeq[s]; }
        sentBring = g_bringSeq;
        SendWord();
        if (!changed) sentAt = now - 100;   // (the repeat: not as fresh)
    }
    unsigned char* mine = MyPlayer();
    // Flying: my player put where the keys take it each tick (through walls - its place written, not walked)
    static bool flying = false;
    static float flyAt[3];
    static LARGE_INTEGER flyLast;
    if (g_fly && mine) {
        float *feet, *shape;
        LARGE_INTEGER t, freq;
        QueryPerformanceCounter(&t);
        QueryPerformanceFrequency(&freq);
        if (PlayerPlace(mine, &feet, &shape)) {
            if (!flying) { memcpy(flyAt, feet, sizeof(flyAt)); flying = true; flyLast = t; Note(L"Flying: W A S D, Space up, Ctrl down, Shift fast"); }
            float dt = (float)(t.QuadPart - flyLast.QuadPart) / (float)freq.QuadPart;
            flyLast = t;
            if (dt > 0.1f) dt = 0.1f;
            RECT game{};
            HWND wnd = DisplayMainWindow();
            bool front = wnd && GetForegroundWindow() == wnd && DisplayWindowRect(&game) && !g_open && !g_freecam;
            LONG testTicks = g_devFlyTest;
            if (front || testTicks > 0) {
                float yaw = CamYaw(mine), speed = (front && Key(VK_SHIFT) ? 30.f : 10.f) * dt;
                float fw[2] = {sinf(yaw), cosf(yaw)}, rt[2] = {cosf(yaw), -sinf(yaw)};
                float mv[3] = {};
                if ((front && Key('W')) || testTicks > 0) { mv[0] += fw[0]; mv[2] += fw[1]; }
                if (front && Key('S')) { mv[0] -= fw[0]; mv[2] -= fw[1]; }
                if (front && Key('D')) { mv[0] += rt[0]; mv[2] += rt[1]; }
                if (front && Key('A')) { mv[0] -= rt[0]; mv[2] -= rt[1]; }
                if (front && Key(VK_SPACE)) mv[1] += 1;
                if (front && Key(VK_CONTROL)) mv[1] -= 1;
                for (int k = 0; k < 3; ++k) flyAt[k] += mv[k] * speed;
                if (testTicks > 0) InterlockedDecrement(&g_devFlyTest);
            }
            PutPlayer(mine, flyAt);
        }
    } else if (flying) {
        flying = false;
        Note(L"Flying off");
    }
    // Enemies held where they stand
    static bool frozeWas = false;
    if (g_freeze || frozeWas) {
        int n = NetplayDevFreeze(g_freeze != 0);
        if (g_freeze && !frozeWas) {
            wchar_t t[80];
            if (n < 0) wcscpy_s(t, L"Freeze enemies: only the host's game can online");
            else swprintf_s(t, L"Enemies frozen (%d now)", n);
            Note(t);
        } else if (!g_freeze) {
            Note(L"Enemies move again");
        }
        frozeWas = g_freeze != 0;
    }
    // The level's lists for the panel (kinds of enemy seen, checkpoints): every second
    static ULONGLONG listsAt = 0;
    if (now - listsAt > 1000) {
        listsAt = now;
        unsigned kinds[48];
        int nk = NetplayDevSpawnKinds(kinds, 24);
        EnterCriticalSection(&g_lock);
        g_kinds.assign(kinds, kinds + nk * 2);
        LeaveCriticalSection(&g_lock);
    }
    LONG action = InterlockedExchange(&g_action, kNone);
    if (action == kNone) return;
    static float saved[3];
    static bool haveSaved = false;
    wchar_t note[160] = L"";
    if (action == kKillAll) {
        int n = NetplayDevKillNpcs();
        if (n < 0) wcscpy_s(note, L"Kill all enemies: only the host's game can online");
        else if (NetplayEnabled()) wcscpy_s(note, L"Every enemy down");
        else swprintf_s(note, L"%d enemies down", n);
    } else if (action == kSavePos || action == kLoadPos) {
        float *feet, *shape;
        if (!mine || !PlayerPlace(mine, &feet, &shape)) {
            wcscpy_s(note, L"No player in a level right now");
        } else if (action == kSavePos) {
            memcpy(saved, feet, sizeof(saved));
            haveSaved = true;
            swprintf_s(note, L"Position saved (%.0f %.0f %.0f)", saved[0], saved[1], saved[2]);
        } else if (!haveSaved) {
            wcscpy_s(note, L"No position saved yet");
        } else {
            PutPlayer(mine, saved);
            if (flying) memcpy(flyAt, saved, sizeof(flyAt));
            wcscpy_s(note, L"Back at the saved position");
        }
    } else if (action == kGoLevel) {
        LONG at = g_level;
        if (at < 0 || at >= (LONG)g_levelPaths.size()) {
            wcscpy_s(note, L"No level chosen");
        } else if (NetplayEnabled() && !(NetplayStoryCoop() && NetplayFreeRun())) {
            wcscpy_s(note, L"Levels: in story co-op or offline only");
        } else {
            const char* path = g_levelPaths[at].c_str();
            if (NetplayEnabled()) NetplayPauseOutcome(3, "GAME_Portal", path);   // (the others: as a level chosen in my pause menu)
            UiRunCommand("GAME_Portal", path);
            haveSaved = false;
            swprintf_s(note, L"Going to %s", g_levelNames[at].c_str());
        }
    } else if (action == kGive) {
        int mask = Targets();
        for (int s = 0; s < 4; ++s) if (mask >> s & 1) InterlockedIncrement(&g_giveSeq[s]);
        swprintf_s(note, L"Every item and power-up given (%s)", g_who == 0 ? L"me" : g_who == 1 ? L"the other player" : L"everyone");
    } else if (action == kBring) {
        float *feet, *shape;
        int other = -1;
        unsigned char* list[8];
        int n = GamePlayers(list, 8);
        for (int i = 0; i < n && other < 0; ++i) { int s = *reinterpret_cast<int*>(list[i] + 0xdf8); if (s != me && Played(list[i])) other = s; }
        if (!mine || !PlayerPlace(mine, &feet, &shape)) wcscpy_s(note, L"No player in a level right now");
        else if (other < 0) wcscpy_s(note, L"No other player here");
        else {
            memcpy(g_bringAt, feet, sizeof(g_bringAt));
            InterlockedExchange(&g_bringTo, other);
            InterlockedIncrement(&g_bringSeq);
            swprintf_s(note, L"Player %d brought here", other + 1);
        }
    } else if (action == kSpawn) {
        float *feet, *shape;
        int n = ChoiceCount(kChKind);
        if (!mine || !PlayerPlace(mine, &feet, &shape)) wcscpy_s(note, L"No player in a level right now");
        else if (n == 0) wcscpy_s(note, L"No kind of enemy seen spawned in this level yet");
        else {
            float yaw = CamYaw(mine), at[3] = {feet[0] + sinf(yaw) * 5, feet[1] + 0.5f, feet[2] + cosf(yaw) * 5};
            int r = NetplayDevSpawn(g_kind % n, at, yaw + 3.14159265f);
            if (r == 1) swprintf_s(note, L"An enemy spawned (%s)", ChoiceText(kChKind).c_str());
            else if (r == -1) wcscpy_s(note, L"Spawn: only the host's game can online");
            else if (r == -2) wcscpy_s(note, L"Spawn: that kind isn't in this level any more");
            else wcscpy_s(note, L"Spawn: the game made none (none free in its pool?)");
        }
    } else if (action == kReload) {
        int n = ModsReloadPacks();
        CosmeticsReloadSkins();
        EmotesRescanNow();
        swprintf_s(note, L"Reloaded: skins and emotes read again, %d mod files looked at (their files: from the next level)", n);
    } else if (action == kRunCommand) {
        EnterCriticalSection(&g_lock);
        std::string line = g_runCmd;
        LeaveCriticalSection(&g_lock);
        if (!line.empty()) {
            VPadCommand(line.c_str());
            swprintf_s(note, L"Ran: %S", line.c_str());
        }
    }
    if (note[0]) Note(note);
}

void DevkitTest(const char* what, int value) {
    bool look = _stricmp(what, "counts") == 0 || _stricmp(what, "drain") == 0 || _stricmp(what, "stat") == 0 || _stricmp(what, "where") == 0;
    if (!g_allowed && !look) { Log(L"Devkit: test - this PC has no key"); return; }   // (the test's looks: any seat)
    Log(L"Devkit: test - %S %d", what, value);
    auto setFlag = [](int flag, int on) {
        int mask = Targets();
        for (int s = 0; s < 4; ++s)
            if (mask >> s & 1) { LONG v = g_flags[s]; InterlockedExchange(&g_flags[s], on ? (v | flag) : (v & ~flag)); }
    };
    if (_stricmp(what, "who") == 0) InterlockedExchange(&g_who, value);
    else if (_stricmp(what, "god") == 0) setFlag(kGod, value);
    else if (_stricmp(what, "ammo") == 0) setFlag(kAmmo, value);
    else if (_stricmp(what, "items") == 0) setFlag(kItems, value);
    else if (_stricmp(what, "give") == 0) InterlockedExchange(&g_action, kGive);
    else if (_stricmp(what, "bring") == 0) InterlockedExchange(&g_action, kBring);
    else if (_stricmp(what, "fly") == 0) InterlockedExchange(&g_fly, value);
    else if (_stricmp(what, "freecam") == 0) InterlockedExchange(&g_freecam, value);
    else if (_stricmp(what, "freeze") == 0) InterlockedExchange(&g_freeze, value);
    else if (_stricmp(what, "kill") == 0) InterlockedExchange(&g_action, kKillAll);
    else if (_stricmp(what, "save") == 0) InterlockedExchange(&g_action, kSavePos);
    else if (_stricmp(what, "load") == 0) InterlockedExchange(&g_action, kLoadPos);
    else if (_stricmp(what, "level") == 0) InterlockedExchange(&g_level, value);
    else if (_stricmp(what, "go") == 0) InterlockedExchange(&g_action, kGoLevel);
    else if (_stricmp(what, "kind") == 0) InterlockedExchange(&g_kind, value);
    else if (_stricmp(what, "spawn") == 0) InterlockedExchange(&g_action, kSpawn);
    else if (_stricmp(what, "reload") == 0) InterlockedExchange(&g_action, kReload);
    else if (_stricmp(what, "show") == 0) PostMessageW(g_panel, WM_APP + 1, value, -1);
    else if (_stricmp(what, "lists") == 0) {
        EnterCriticalSection(&g_lock);
        Log(L"Devkit: test - %d kinds of enemy", (int)g_kinds.size() / 2);
        for (size_t i = 0; i + 1 < g_kinds.size(); i += 2) Log(L"Devkit: enemy kind %d: %x:%x", (int)(i / 2), g_kinds[i], g_kinds[i + 1]);
        LeaveCriticalSection(&g_lock);
    }
    else if (_stricmp(what, "stat") == 0) {
        Log(L"Devkit: test - fills %ld, writes %ld", g_fills, g_fillWrites);
        Log(L"Devkit: test - %ld ticks so far; me slot %d; flags %lx %lx %lx %lx; fly %ld freecam %ld freeze %ld", g_ticks, MySlot(),
            g_flags[0], g_flags[1], g_flags[2], g_flags[3], g_fly, g_freecam, g_freeze);
        unsigned char* list[8];
        int n = GamePlayers(list, 8);
        for (int i = 0; i < n; ++i)
            Log(L"Devkit: test - player %p slot %d view %d", list[i], *reinterpret_cast<int*>(list[i] + 0xdf8), *reinterpret_cast<int*>(list[i] + 0xdfc));
    }
    else if (_stricmp(what, "where") == 0) {
        for (int s = 0; s < 4; ++s) {
            float *feet, *shape;
            unsigned char* p = PlayerOf(s);
            if (p && PlayerPlace(p, &feet, &shape)) Log(L"Devkit: test - player %d at (%.2f %.2f %.2f)", s, feet[0], feet[1], feet[2]);
        }
    }
    else if (_stricmp(what, "counts") == 0) {   // research / test: my player's weapons and items as they are
        unsigned char* p = PlayerOf(value >= 10 && value < 14 ? value - 10 : MySlot());   // ("counts 11": player 1's)
        if (!p) { Log(L"Devkit: test - no player"); return; }
        ForWeapons(p, [](unsigned char* w) {
            Log(L"Devkit: test - weapon %x:%x %d / %d (clip of %d)%s", *reinterpret_cast<unsigned*>(w + 0x18), *reinterpret_cast<unsigned*>(w + 0x1c),
                *reinterpret_cast<int*>(w + 0x164), *reinterpret_cast<int*>(w + 0x168), *reinterpret_cast<int*>(w + 0x170), IsItem(w) ? L" - an item" : L"");
        });
        if (float* hp = HealthOf(p)) Log(L"Devkit: test - health %.1f", *hp);
    }
    else if (_stricmp(what, "drain") == 0) {   // test: my player's (or that one's) every count down to 0 / 1 - as if all used
        unsigned char* p = PlayerOf(value >= 10 && value < 14 ? value - 10 : MySlot());
        if (p) ForWeapons(p, [](unsigned char* w) { *reinterpret_cast<int*>(w + 0x164) = 0; });
    }
    else if (_stricmp(what, "flytest") == 0) InterlockedExchange(&g_devFlyTest, value);   // test: as W held for that many ticks while flying
}
