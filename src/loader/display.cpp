// Display modes: fullscreen, windowed, and borderless "windowed fullscreen".
//
// The game reads ScreenWidth / ScreenHeight / ScreenWindowed (4-byte binary values) from
// HKCU\Software\Heavy Iron Studios Inc.\Family Guy\Settings. We override those reads; the user's
// saved values are never changed by us (the game itself saves them when you change Options).
//
// Borderless = the game runs in windowed mode, and we make its main window a captionless popup that
// covers the whole monitor, stop it from adding a border back or moving/resizing itself, and swallow
// its display-mode changes. It always renders at the monitor's own resolution (the game's resolution
// choice is overridden), so the picture, the window and the mouse all agree.
//
// The mode can also be switched from the game's own Options > Screen > Display Mode list (which our
// menu patch extends with "Borderless"; see ui.cpp). The choice is saved to FGUYMod.ini.
//
// Settings (FGUYMod\FGUYMod.ini, [Display]) — environment variables override them for test runs:
//   Mode   = default | fullscreen | windowed | borderless      (env FGUYMOD_MODE; FGUYMOD_WINDOWED=1)
//   (env FGUYMOD_RES=WxH: a render size for test runs, any mode)
//   RunInBackground = 1 keeps the game running when it isn't the active window (env FGUYMOD_BACKGROUND)
//
// Online sessions boot hidden (handoff.cpp): the main window is created invisible and stays so until
// DisplayReveal. Such a session can't use exclusive fullscreen (a hidden window can't own the screen, and
// the old game still shows the loading screen), so fullscreen becomes borderless at the same resolution;
// it also keeps running in the background (lockstep needs every game ticking).

#include "common.h"

#include <cwchar>
#include <initializer_list>

namespace {

enum class Mode { Default, Fullscreen, Windowed, Borderless };
Mode g_mode = Mode::Default;
int g_width = 0, g_height = 0;   // render size override (0 = none)
bool g_envRes = false;           // g_width/g_height came from FGUYMOD_RES (applies in every mode)
int g_gameWidth = 0, g_gameHeight = 0;  // what the game is rendering at, as far as we know
int g_gameWindowed = -1;         // the game's own ScreenWindowed setting (-1 = not read yet)
RECT g_monitor;                  // physical pixels
HWND g_mainWnd = nullptr;
LONG g_windowedStyle = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
bool g_background = false;       // keep running when not the active window
bool g_padMode = false;          // report controller mode instead of keyboard/mouse (test runs)
WNDPROC g_gameWndProc = nullptr;
Mode g_menuChoice = Mode::Default;  // picked in Options > Screen, applied on OK
bool g_testRun = false;          // mode forced by an environment variable: don't save choices
bool g_hidden = false;           // online session booting hidden until DisplayReveal (handoff.cpp)
bool g_noFullscreen = false;     //   and therefore borderless instead of fullscreen

// Order of the choices in the (patched) Options > Screen > Display Mode list.
const int kMenuWindowed = 0, kMenuFullscreen = 1, kMenuBorderless = 2;

const LONG kBorderStyles = WS_CAPTION | WS_THICKFRAME | WS_SYSMENU | WS_MINIMIZEBOX | WS_MAXIMIZEBOX |
                           WS_BORDER | WS_DLGFRAME;
const LONG kBorderExStyles = WS_EX_DLGMODALFRAME | WS_EX_CLIENTEDGE | WS_EX_STATICEDGE |
                             WS_EX_WINDOWEDGE | WS_EX_TOPMOST;

const wchar_t* ModeName(Mode m) {
    switch (m) {
        case Mode::Fullscreen: return L"fullscreen";
        case Mode::Windowed: return L"windowed";
        case Mode::Borderless: return L"borderless";
        default: return L"default (game settings)";
    }
}

// The mode actually in effect ("default" resolves to whatever the game's own setting says).
Mode EffectiveMode() {
    if (g_mode != Mode::Default) return g_mode;
    return g_gameWindowed == 1 ? Mode::Windowed : Mode::Fullscreen;
}

bool IsGameMainWindowClass(LPCWSTR cls) {
    return cls && !IS_INTRESOURCE(cls) && wcsstr(cls, L"GameWindow") != nullptr;
}

int MonitorW() { return g_monitor.right - g_monitor.left; }
int MonitorH() { return g_monitor.bottom - g_monitor.top; }

// ---- registry: what the game thinks its settings are ------------------------------------------

decltype(&RegQueryValueExW) Real_RegQueryValueExW;

LSTATUS WINAPI Hook_RegQueryValueExW(HKEY key, LPCWSTR name, LPDWORD reserved, LPDWORD type,
                                     LPBYTE data, LPDWORD size) {
    LSTATUS r = Real_RegQueryValueExW(key, name, reserved, type, data, size);
    if (r != ERROR_SUCCESS || !name || !data || !size) return r;
    if (g_padMode && *size >= 1 && _wcsicmp(name, L"IsKeyboardMouseInput") == 0) {
        data[0] = 0;  // 1-byte flag
        Log(L"Input: game setting IsKeyboardMouseInput -> 0 (controller mode)");
        return r;
    }
    if (*size < 4) return r;
    int value = -1, stored;
    memcpy(&stored, data, 4);
    bool sizeOverride = g_width > 0 && (g_envRes || g_mode == Mode::Borderless);
    if (_wcsicmp(name, L"ScreenWindowed") == 0) {
        g_gameWindowed = stored ? 1 : 0;
        if (g_noFullscreen && g_mode == Mode::Default && !stored) {
            g_mode = Mode::Borderless;
            if (!g_envRes) { g_width = MonitorW(); g_height = MonitorH(); }
            Log(L"Display: online session - borderless instead of fullscreen");
        }
        if (g_mode != Mode::Default) value = g_mode == Mode::Fullscreen ? 0 : 1;
    } else if (_wcsicmp(name, L"ScreenWidth") == 0) {
        g_gameWidth = sizeOverride ? g_width : stored;
        if (sizeOverride) value = g_width;
    } else if (_wcsicmp(name, L"ScreenHeight") == 0) {
        g_gameHeight = sizeOverride ? g_height : stored;
        if (sizeOverride) value = g_height;
    }
    if (value >= 0) {
        memcpy(data, &value, 4);
        Log(L"Display: game setting %s -> %d", name, value);
    }
    return r;
}

// Test runs must not touch the player's saved settings (the game writes back what it was told).
decltype(&RegSetValueExW) Real_RegSetValueExW;

LSTATUS WINAPI Hook_RegSetValueExW(HKEY key, LPCWSTR name, DWORD reserved, DWORD type, const BYTE* data,
                                   DWORD size) {
    if (g_testRun) {
        Log(L"Display: test run - not saving game setting %s", name ? name : L"(default)");
        return ERROR_SUCCESS;
    }
    return Real_RegSetValueExW(key, name, reserved, type, data, size);
}

// ---- our window procedure (wraps the game's) -------------------------------------------------
// * Run in background: the game pauses itself when its window loses focus, so we hide focus loss.
// * Restyle requests: menu calls arrive on the game's render thread, and changing a window from
//   another thread waits on the window's own thread (which may be waiting on the render thread:
//   deadlock). So the menu posts kRestyleMsg and the restyle happens here, on the window's thread.

const UINT kRestyleMsg = WM_APP + 0x4647;  // wParam = target Mode
const UINT kRevealMsg = WM_APP + 0x4648;   // show the hidden session window (handoff.cpp)
const UINT kHideMsg = WM_APP + 0x4649;     // hide the window (the old game, while it closes)
void RestyleWindow(Mode to);

LRESULT CALLBACK ModWndProc(HWND wnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_KEYDOWN && !NetplayEnabled()) OnlineMenuKey(wnd, wp);  // typing on the ONLINE screens
    LRESULT handled;
    if (KbmOnMessage(wnd, msg, wp, lp, &handled)) return handled;
    switch (msg) {
        case kRestyleMsg:
            RestyleWindow(static_cast<Mode>(wp));
            return 0;
        case kRevealMsg:
            if (IsBot()) { HandoffRevealed(); return 0; }   // the test bot never shows (the old copy can close)
            if (g_hidden) {
                g_hidden = false;
                ShowWindow(wnd, SW_SHOW);
                SetWindowPos(wnd, HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW);
                SetForegroundWindow(wnd);
                HandoffRevealed();
            }
            return 0;
        case kHideMsg:
            ShowWindow(wnd, SW_HIDE);
            return 0;
        case WM_WINDOWPOSCHANGED:
            // In windowed mode the game takes its screen size from the window's client area here
            // (FGUY.exe 0x48399a; its window procedure handles only WM_DESTROY, WM_ACTIVATE,
            // WM_CLOSE and this). Borderless: the window is monitor-sized but the game renders at its
            // own resolution (Direct3D stretches it), so show the game a copy carrying that size.
            // Otherwise it thinks the resolution changed, and its "keep this resolution?" timer
            // later reverts to the monitor size. Applies to SWP_NOSIZE messages too: they still
            // carry the current size, and the game reads it regardless of the flag.
            if (g_mode == Mode::Borderless && lp && g_gameWidth > 0 && g_gameHeight > 0) {
                WINDOWPOS copy = *reinterpret_cast<WINDOWPOS*>(lp);
                copy.cx = g_gameWidth;
                copy.cy = g_gameHeight;
                return CallWindowProcW(g_gameWndProc, wnd, msg, wp, reinterpret_cast<LPARAM>(&copy));
            }
            break;
        case WM_ACTIVATEAPP:
            if (g_background && !wp) return 0;
            break;
        case WM_ACTIVATE:
            if (g_background && LOWORD(wp) == WA_INACTIVE) return 0;
            break;
        case WM_KILLFOCUS:
            if (g_background) return 0;
            break;
    }
    return CallWindowProcW(g_gameWndProc, wnd, msg, wp, lp);
}

// ---- the mouse cursor in borderless mode -----------------------------------------------------------
// Borderless at a render size below the monitor's (e.g. 1920x1080 on a 4K screen), the game believes its
// window is render-sized (GetClientRect / GetWindowRect / WM_WINDOWPOSCHANGED below), but Windows reports
// the cursor in real screen pixels - so the game drew its cursor at the wrong place, mostly off screen. Its
// cursor reads and cursor clipping are translated between the two: positions it reads are scaled to its
// size, rectangles it clips the cursor to are scaled back up to the screen.
bool CursorScaled() {
    return g_mode == Mode::Borderless && g_gameWidth > 0 && g_gameHeight > 0 &&
           (g_gameWidth != MonitorW() || g_gameHeight != MonitorH());
}

decltype(&GetCursorPos) Real_GetCursorPos;
decltype(&GetWindowRect) Real_GetWindowRect;

BOOL WINAPI Hook_GetCursorPos(POINT* pt) {
    BOOL ok = Real_GetCursorPos(pt);
    if (ok && pt && CursorScaled()) {
        pt->x = g_monitor.left + MulDiv(pt->x - g_monitor.left, g_gameWidth, MonitorW());
        pt->y = g_monitor.top + MulDiv(pt->y - g_monitor.top, g_gameHeight, MonitorH());
    }
    return ok;
}

BOOL WINAPI Hook_GetWindowRect(HWND wnd, LPRECT rect) {
    BOOL ok = Real_GetWindowRect(wnd, rect);
    if (ok && rect && wnd && wnd == g_mainWnd && CursorScaled())
        SetRect(rect, g_monitor.left, g_monitor.top, g_monitor.left + g_gameWidth, g_monitor.top + g_gameHeight);
    return ok;
}

decltype(&ClipCursor) Real_ClipCursor;

BOOL WINAPI Hook_ClipCursor(const RECT* rect) {
    // The game thinks it's always active, so don't let it trap the mouse while you're elsewhere.
    if (rect && g_background && GetForegroundWindow() != g_mainWnd) return Real_ClipCursor(nullptr);
    if (rect && CursorScaled()) {  // the game's rectangle is in its own (render-sized) space
        RECT r;
        r.left = g_monitor.left + MulDiv(rect->left - g_monitor.left, MonitorW(), g_gameWidth);
        r.top = g_monitor.top + MulDiv(rect->top - g_monitor.top, MonitorH(), g_gameHeight);
        r.right = g_monitor.left + MulDiv(rect->right - g_monitor.left, MonitorW(), g_gameWidth);
        r.bottom = g_monitor.top + MulDiv(rect->bottom - g_monitor.top, MonitorH(), g_gameHeight);
        return Real_ClipCursor(&r);
    }
    return Real_ClipCursor(rect);
}

// ---- window hooks (only change anything while borderless) ------------------------------------

decltype(&CreateWindowExW) Real_CreateWindowExW;
decltype(&SetWindowLongW) Real_SetWindowLongW;
decltype(&SetWindowPos) Real_SetWindowPos;
decltype(&AdjustWindowRect) Real_AdjustWindowRect;
decltype(&ChangeDisplaySettingsW) Real_ChangeDisplaySettingsW;
decltype(&ShowWindow) Real_ShowWindow;

HWND WINAPI Hook_CreateWindowExW(DWORD ex, LPCWSTR cls, LPCWSTR title, DWORD style, int x, int y,
                                 int w, int h, HWND parent, HMENU menu, HINSTANCE inst, LPVOID param) {
    bool main = !(style & WS_CHILD) && IsGameMainWindowClass(cls);
    if (main && (style & WS_CAPTION)) g_windowedStyle = (LONG)style;
    if (main && g_hidden) style &= ~WS_VISIBLE;
    if (main && g_mode == Mode::Borderless) {
        style = (style & ~kBorderStyles) | WS_POPUP;
        ex &= ~kBorderExStyles;
        x = g_monitor.left;
        y = g_monitor.top;
        w = MonitorW();
        h = MonitorH();
    }
    HWND wnd = Real_CreateWindowExW(ex, cls, title, style, x, y, w, h, parent, menu, inst, param);
    Log(L"Window: created \"%s\" class=%s style=%08x at %d,%d %dx%d%s",
        title ? title : L"", IS_INTRESOURCE(cls) ? L"(atom)" : cls, style, x, y, w, h,
        main ? L"  [main window]" : L"");
    if (main && wnd) {
        g_mainWnd = wnd;
        g_gameWndProc = (WNDPROC)SetWindowLongW(wnd, GWL_WNDPROC, (LONG)(LONG_PTR)ModWndProc);
        if (g_background) Log(L"Window: will keep running in the background");
        KbmInit(wnd);  // online sessions: keyboard and mouse as this player's controller
    }
    return wnd;
}

LONG WINAPI Hook_SetWindowLongW(HWND wnd, int index, LONG value) {
    if (wnd && wnd == g_mainWnd && (index == GWL_STYLE || index == GWL_EXSTYLE))
        Log(L"Window: game sets %s %08x (mode %s)", index == GWL_STYLE ? L"style" : L"exstyle", value,
            ModeName(g_mode));
    if (g_mode == Mode::Borderless && wnd && wnd == g_mainWnd) {
        if (index == GWL_STYLE) value = (value & ~kBorderStyles) | WS_POPUP;
        if (index == GWL_EXSTYLE) value &= ~kBorderExStyles;
    }
    return Real_SetWindowLongW(wnd, index, value);
}

BOOL WINAPI Hook_ShowWindow(HWND wnd, int cmd) {
    if (g_hidden && wnd && wnd == g_mainWnd && cmd != SW_HIDE) {
        Log(L"Window: staying hidden (online session still loading)");
        return FALSE;
    }
    return Real_ShowWindow(wnd, cmd);
}

BOOL WINAPI Hook_SetWindowPos(HWND wnd, HWND after, int x, int y, int cx, int cy, UINT flags) {
    if (wnd && wnd == g_mainWnd)
        Log(L"Window: game moves window to %d,%d %dx%d flags %#x (mode %s)", x, y, cx, cy, flags, ModeName(g_mode));
    if (g_hidden && wnd && wnd == g_mainWnd) flags &= ~SWP_SHOWWINDOW;
    if (g_mode == Mode::Borderless && wnd && wnd == g_mainWnd) {
        x = g_monitor.left;
        y = g_monitor.top;
        cx = MonitorW();
        cy = MonitorH();
        flags &= ~(SWP_NOMOVE | SWP_NOSIZE);
        if (after == HWND_TOPMOST) after = HWND_TOP;  // stay alt-tab friendly
        flags |= SWP_FRAMECHANGED;
    }
    return Real_SetWindowPos(wnd, after, x, y, cx, cy, flags);
}

decltype(&GetClientRect) Real_GetClientRect;

BOOL WINAPI Hook_GetClientRect(HWND wnd, LPRECT rect) {
    // Borderless: the game sizes its rendering from this; keep it at the render size (see WM_SIZE).
    if (g_mode == Mode::Borderless && wnd && wnd == g_mainWnd && rect && g_gameWidth > 0 && g_gameHeight > 0) {
        SetRect(rect, 0, 0, g_gameWidth, g_gameHeight);
        return TRUE;
    }
    return Real_GetClientRect(wnd, rect);
}

BOOL WINAPI Hook_AdjustWindowRect(LPRECT rect, DWORD style, BOOL menu) {
    // The game uses this to grow its window for the border; borderless has none.
    if (g_mode == Mode::Borderless) return TRUE;
    return Real_AdjustWindowRect(rect, style, menu);
}

LONG WINAPI Hook_ChangeDisplaySettingsW(DEVMODEW* mode, DWORD flags) {
    if (g_mode == Mode::Borderless) {
        Log(L"Display: blocked a display mode change (%s)", mode ? L"set mode" : L"restore");
        return DISP_CHANGE_SUCCESSFUL;
    }
    return Real_ChangeDisplaySettingsW(mode, flags);
}

void MakeDpiAware() {
    // Without this, Windows scales the window on high-DPI screens (blurry, wrong size).
    using SetCtx_t = BOOL(WINAPI*)(HANDLE);
    HMODULE user32 = GetModuleHandleW(L"user32.dll");
    auto setCtx = (SetCtx_t)GetProcAddress(user32, "SetProcessDpiAwarenessContext");
    BOOL ok = FALSE;
    if (setCtx) ok = setCtx((HANDLE)-4 /* DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2 */);
    if (!ok) ok = SetProcessDPIAware();
    Log(L"Display: DPI aware %s", ok ? L"on" : L"FAILED");
}

// Restyle the live window when switching between windowed and borderless (the game's windowed
// flag doesn't change, so it won't do anything itself).
void RestyleWindow(Mode to) {
    if (!g_mainWnd) return;
    if (to == Mode::Borderless) {
        LONG style = (GetWindowLongW(g_mainWnd, GWL_STYLE) & ~kBorderStyles) | WS_POPUP | WS_VISIBLE;
        SetWindowLongW(g_mainWnd, GWL_STYLE, style);
        SetWindowLongW(g_mainWnd, GWL_EXSTYLE, GetWindowLongW(g_mainWnd, GWL_EXSTYLE) & ~kBorderExStyles);
        SetWindowPos(g_mainWnd, HWND_TOP, g_monitor.left, g_monitor.top, MonitorW(), MonitorH(),
                     SWP_FRAMECHANGED | SWP_SHOWWINDOW);
    } else {
        LONG style = (g_windowedStyle & ~WS_POPUP) | WS_VISIBLE;
        SetWindowLongW(g_mainWnd, GWL_STYLE, style);
        int w = g_gameWidth > 0 ? g_gameWidth : 1280, h = g_gameHeight > 0 ? g_gameHeight : 720;
        RECT r = {0, 0, w, h};
        AdjustWindowRect(&r, style, FALSE);
        int ww = r.right - r.left, wh = r.bottom - r.top;
        SetWindowPos(g_mainWnd, HWND_NOTOPMOST, g_monitor.left + (MonitorW() - ww) / 2,
                     g_monitor.top + (MonitorH() - wh) / 2, ww, wh, SWP_FRAMECHANGED | SWP_SHOWWINDOW);
    }
}

void SaveMode(Mode m) {
    if (g_testRun) return;
    wchar_t path[MAX_PATH];
    swprintf_s(path, L"%s\\FGUYMod.ini", g_modDir);
    WritePrivateProfileStringW(L"Display", L"Mode", ModeName(m), path);
}

}  // namespace

// A point in the game window's client area (real pixels) as the game sees it (its render size).
void DisplayClientToGame(POINT* pt) {
    if (!pt || !CursorScaled()) return;
    pt->x = MulDiv(pt->x, g_gameWidth, MonitorW());
    pt->y = MulDiv(pt->y, g_gameHeight, MonitorH());
}

bool DisplayIsWindowed() { Mode m = EffectiveMode(); return m == Mode::Windowed || m == Mode::Borderless; }

// ---- Options > Screen > Display Mode (called from the patched menu, see ui.cpp) -----------------

int DisplayMenuIndex() {
    // While the Screen page is open, the list shows the pending choice (like the original script,
    // which read its own needWindowed variable).
    switch (g_menuChoice != Mode::Default ? g_menuChoice : EffectiveMode()) {
        case Mode::Borderless: return kMenuBorderless;
        case Mode::Windowed: return kMenuWindowed;
        default: return kMenuFullscreen;
    }
}

bool DisplayMenuSelect(int index) {
    g_menuChoice = index == kMenuBorderless ? Mode::Borderless
                 : index == kMenuWindowed   ? Mode::Windowed
                                            : Mode::Fullscreen;
    Log(L"Display: menu choice %d (%s)", index, ModeName(g_menuChoice));
    return g_menuChoice != Mode::Fullscreen;  // the game's own "windowed" flag
}

void DisplayMenuApply() {
    Mode from = EffectiveMode(), to = g_menuChoice;
    g_menuChoice = Mode::Default;
    if (to == Mode::Default || to == from) return;
    Log(L"Display: switching %s -> %s", ModeName(from), ModeName(to));
    g_mode = to;
    SaveMode(to);
    // Windowed <-> borderless: same windowed flag for the game, so we restyle the window ourselves.
    // Anything involving fullscreen: the menu goes on to call GRFX_ChangeResolution, and the game
    // switches itself (with our hooks shaping the window if the target is borderless).
    if (from != Mode::Fullscreen && to != Mode::Fullscreen && g_mainWnd)
        PostMessageW(g_mainWnd, kRestyleMsg, static_cast<WPARAM>(to), 0);
}

void DisplayMenuReset() { g_menuChoice = Mode::Default; }

// Online session: show the hidden window (handoff.cpp). Done on the window's own thread.
void DisplayReveal() {
    if (g_mainWnd) PostMessageW(g_mainWnd, kRevealMsg, 0, 0);
    else g_hidden = false;
}

void DisplayHideWindow() {
    if (g_mainWnd) PostMessageW(g_mainWnd, kHideMsg, 0, 0);
}

// The menu changes the resolution (ui.cpp, before the game does): borderless always uses the monitor's
// own resolution, whatever was picked. Returns true if the size was changed.
bool DisplayOnChangeResolution(int* width, int* height, bool windowed) {
    bool borderless = g_menuChoice == Mode::Borderless || (g_menuChoice == Mode::Default && g_mode == Mode::Borderless);
    bool changed = false;
    if (borderless && windowed && !g_envRes && (*width != MonitorW() || *height != MonitorH())) {
        *width = MonitorW();
        *height = MonitorH();
        changed = true;
    }
    Log(L"Display: game changes resolution to %dx%d %s (mode %s)%s", *width, *height,
        windowed ? L"windowed" : L"fullscreen", ModeName(EffectiveMode()), changed ? L" - borderless: the monitor's own" : L"");
    g_gameWidth = *width;
    g_gameHeight = *height;
    g_gameWindowed = windowed ? 1 : 0;
    return changed;
}

// Research aid ([Debug] WatchScreenSize=1): log every change of the game's screen-size globals.
DWORD WINAPI WatchScreenSize(LPVOID) {
    volatile int* w = reinterpret_cast<int*>(0xb06360);
    volatile int* h = reinterpret_cast<int*>(0xb0635c);
    int lw = -1, lh = -1;
    for (;;) {
        if (*w != lw || *h != lh) {
            lw = *w;
            lh = *h;
            Log(L"Watch: game screen size is now %dx%d (mode %s)", lw, lh, ModeName(g_mode));
        }
        Sleep(5);
    }
}

void InitDisplay(HMODULE exe) {
    if (SettingInt(L"Debug", L"WatchScreenSize", L"FGUYMOD_WATCH", 0))
        CloseHandle(CreateThread(nullptr, 0, WatchScreenSize, nullptr, 0, nullptr));
    wchar_t mode[32];
    SettingString(L"Display", L"Mode", L"FGUYMOD_MODE", L"default", mode, 32);
    if (_wcsicmp(mode, L"fullscreen") == 0) g_mode = Mode::Fullscreen;
    else if (_wcsicmp(mode, L"windowed") == 0) g_mode = Mode::Windowed;
    else if (_wcsicmp(mode, L"borderless") == 0) g_mode = Mode::Borderless;
    if (SettingInt(L"Display", L"_", L"FGUYMOD_WINDOWED", 0)) g_mode = Mode::Windowed;  // test scripts
    wchar_t env[8];
    for (const wchar_t* v : {L"FGUYMOD_MODE", L"FGUYMOD_WINDOWED", L"FGUYMOD_RES", L"FGUYMOD_PADMODE", L"FGUYMOD_NET"})
        g_testRun |= GetEnvironmentVariableW(v, env, 8) != 0;

    g_width = 0;   // (older versions kept a borderless render size in [Display] Width / Height: no longer used)
    g_height = 0;
    wchar_t res[32];
    SettingString(L"Display", L"_", L"FGUYMOD_RES", L"", res, 32);
    if (res[0] && swscanf_s(res, L"%dx%d", &g_width, &g_height) == 2) g_envRes = true;
    g_background = SettingInt(L"Display", L"RunInBackground", L"FGUYMOD_BACKGROUND", 0) != 0;
    g_padMode = SettingInt(L"Debug", L"PadMode", L"FGUYMOD_PADMODE", 0) != 0;

    // Always DPI aware, so switching to windowed/borderless from the in-game menu stays sharp.
    MakeDpiAware();
    MONITORINFO mi = {sizeof(mi)};
    GetMonitorInfoW(MonitorFromPoint({0, 0}, MONITOR_DEFAULTTOPRIMARY), &mi);
    g_monitor = mi.rcMonitor;
    if (g_mode == Mode::Borderless && !g_envRes) {   // borderless: the monitor's own resolution
        g_width = MonitorW();
        g_height = MonitorH();
    }
    if (IsBot()) {   // the test bot runs hidden in the background
        g_hidden = true;
        g_background = true;
    }
    if (HandoffHidden()) {
        g_hidden = true;
        g_noFullscreen = true;
        g_background = true;
        if (g_noFullscreen && !g_envRes) { g_width = MonitorW(); g_height = MonitorH(); }  // if it goes borderless
        if (g_mode == Mode::Fullscreen) {
            g_mode = Mode::Borderless;
            Log(L"Display: online session - borderless instead of fullscreen");
        }
    }
    Log(L"Display: mode %s, render size %s%dx%d, run in background %s", ModeName(g_mode),
        g_width > 0 ? L"" : L"(game setting) ", g_width, g_height, g_background ? L"on" : L"off");

    HookSpec hooks[] = {
        {"advapi32.dll", "RegQueryValueExW", (void*)Hook_RegQueryValueExW, (void**)&Real_RegQueryValueExW},
        {"advapi32.dll", "RegSetValueExW", (void*)Hook_RegSetValueExW, (void**)&Real_RegSetValueExW},
        {"user32.dll", "CreateWindowExW", (void*)Hook_CreateWindowExW, (void**)&Real_CreateWindowExW},
        {"user32.dll", "SetWindowLongW", (void*)Hook_SetWindowLongW, (void**)&Real_SetWindowLongW},
        {"user32.dll", "SetWindowPos", (void*)Hook_SetWindowPos, (void**)&Real_SetWindowPos},
        {"user32.dll", "AdjustWindowRect", (void*)Hook_AdjustWindowRect, (void**)&Real_AdjustWindowRect},
        {"user32.dll", "GetClientRect", (void*)Hook_GetClientRect, (void**)&Real_GetClientRect},
        {"user32.dll", "ChangeDisplaySettingsW", (void*)Hook_ChangeDisplaySettingsW, (void**)&Real_ChangeDisplaySettingsW},
        {"user32.dll", "ShowWindow", (void*)Hook_ShowWindow, (void**)&Real_ShowWindow},
    };
    InstallHooks(exe, hooks);

    HookSpec cursorHooks[] = {  // borderless cursor scaling, and background mode's cursor release
        {"user32.dll", "ClipCursor", (void*)Hook_ClipCursor, (void**)&Real_ClipCursor},
        {"user32.dll", "GetCursorPos", (void*)Hook_GetCursorPos, (void**)&Real_GetCursorPos},
        {"user32.dll", "GetWindowRect", (void*)Hook_GetWindowRect, (void**)&Real_GetWindowRect},
    };
    InstallHooks(exe, cursorHooks);
}
