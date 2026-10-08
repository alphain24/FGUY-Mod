// Display modes: fullscreen, windowed, and borderless "windowed fullscreen".
//
// The game reads ScreenWidth / ScreenHeight / ScreenWindowed (4-byte binary values) from
// HKCU\Software\Heavy Iron Studios Inc.\Family Guy\Settings. We override those reads; the user's
// saved values are never changed.
//
// Borderless = the game runs in windowed mode, and we make its main window a captionless popup that
// covers the whole monitor, stop it from adding a border back or moving/resizing itself, and swallow
// its display-mode changes. If the render resolution is lower than the monitor's, Direct3D stretches
// the picture to fill the window.
//
// Settings (FGUYMod\FGUYMod.ini, [Display]) — environment variables override them for test runs:
//   Mode   = default | fullscreen | windowed | borderless      (env FGUYMOD_MODE; FGUYMOD_WINDOWED=1)
//   Width  = render width,  0 = monitor's native width          (env FGUYMOD_RES=WxH)
//   Height = render height, 0 = monitor's native height
//   RunInBackground = 1 keeps the game running when it isn't the active window (env FGUYMOD_BACKGROUND)

#include "common.h"

#include <cwchar>

namespace {

enum class Mode { Default, Fullscreen, Windowed, Borderless };
Mode g_mode = Mode::Default;
int g_width = 0, g_height = 0;  // render resolution override (0 = none / native for borderless)
RECT g_monitor;                 // physical pixels
HWND g_mainWnd = nullptr;
bool g_background = false;      // keep running when not the active window
bool g_padMode = false;         // report controller mode instead of keyboard/mouse (test runs)
WNDPROC g_gameWndProc = nullptr;

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

bool IsGameMainWindowClass(LPCWSTR cls) {
    return cls && !IS_INTRESOURCE(cls) && wcsstr(cls, L"GameWindow") != nullptr;
}

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
    int value = -1;
    if (g_mode != Mode::Default && _wcsicmp(name, L"ScreenWindowed") == 0)
        value = g_mode == Mode::Fullscreen ? 0 : 1;
    if (g_width > 0 && _wcsicmp(name, L"ScreenWidth") == 0) value = g_width;
    if (g_height > 0 && _wcsicmp(name, L"ScreenHeight") == 0) value = g_height;
    if (value >= 0) {
        memcpy(data, &value, 4);
        Log(L"Display: game setting %s -> %d", name, value);
    }
    return r;
}

// ---- window hooks (borderless only) -----------------------------------------------------------

decltype(&CreateWindowExW) Real_CreateWindowExW;
decltype(&SetWindowLongW) Real_SetWindowLongW;
decltype(&SetWindowPos) Real_SetWindowPos;
decltype(&AdjustWindowRect) Real_AdjustWindowRect;
decltype(&ChangeDisplaySettingsW) Real_ChangeDisplaySettingsW;

// ---- run in background ------------------------------------------------------------------------
// The game pauses itself when its window loses focus. We hide focus loss from its window procedure.

LRESULT CALLBACK BackgroundWndProc(HWND wnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_ACTIVATEAPP:
            if (!wp) return 0;
            break;
        case WM_ACTIVATE:
            if (LOWORD(wp) == WA_INACTIVE) return 0;
            break;
        case WM_KILLFOCUS:
            return 0;
    }
    return CallWindowProcW(g_gameWndProc, wnd, msg, wp, lp);
}

decltype(&ClipCursor) Real_ClipCursor;

BOOL WINAPI Hook_ClipCursor(const RECT* rect) {
    // The game thinks it's always active, so don't let it trap the mouse while you're elsewhere.
    if (rect && GetForegroundWindow() != g_mainWnd) return ClipCursor(nullptr);
    return Real_ClipCursor(rect);
}

// ---- window hooks ----------------------------------------------------------------------------

HWND WINAPI Hook_CreateWindowExW(DWORD ex, LPCWSTR cls, LPCWSTR title, DWORD style, int x, int y,
                                 int w, int h, HWND parent, HMENU menu, HINSTANCE inst, LPVOID param) {
    bool main = !(style & WS_CHILD) && IsGameMainWindowClass(cls);
    if (main && g_mode == Mode::Borderless) {
        style = (style & ~kBorderStyles) | WS_POPUP;
        ex &= ~kBorderExStyles;
        x = g_monitor.left;
        y = g_monitor.top;
        w = g_monitor.right - g_monitor.left;
        h = g_monitor.bottom - g_monitor.top;
    }
    HWND wnd = Real_CreateWindowExW(ex, cls, title, style, x, y, w, h, parent, menu, inst, param);
    Log(L"Window: created \"%s\" class=%s style=%08x at %d,%d %dx%d%s",
        title ? title : L"", IS_INTRESOURCE(cls) ? L"(atom)" : cls, style, x, y, w, h,
        main ? L"  [main window]" : L"");
    if (main && wnd) {
        g_mainWnd = wnd;
        if (g_background) {
            g_gameWndProc = (WNDPROC)SetWindowLongW(wnd, GWL_WNDPROC, (LONG)(LONG_PTR)BackgroundWndProc);
            Log(L"Window: will keep running in the background");
        }
    }
    return wnd;
}

LONG WINAPI Hook_SetWindowLongW(HWND wnd, int index, LONG value) {
    if (g_mode == Mode::Borderless && wnd && wnd == g_mainWnd) {
        if (index == GWL_STYLE) value = (value & ~kBorderStyles) | WS_POPUP;
        if (index == GWL_EXSTYLE) value &= ~kBorderExStyles;
    }
    return Real_SetWindowLongW(wnd, index, value);
}

BOOL WINAPI Hook_SetWindowPos(HWND wnd, HWND after, int x, int y, int cx, int cy, UINT flags) {
    if (g_mode == Mode::Borderless && wnd && wnd == g_mainWnd) {
        x = g_monitor.left;
        y = g_monitor.top;
        cx = g_monitor.right - g_monitor.left;
        cy = g_monitor.bottom - g_monitor.top;
        flags &= ~(SWP_NOMOVE | SWP_NOSIZE);
        if (after == HWND_TOPMOST) after = HWND_TOP;  // stay alt-tab friendly
        flags |= SWP_FRAMECHANGED;
    }
    return Real_SetWindowPos(wnd, after, x, y, cx, cy, flags);
}

BOOL WINAPI Hook_AdjustWindowRect(LPRECT, DWORD, BOOL) {
    // The game uses this to grow its window for the border; borderless has none.
    return TRUE;
}

LONG WINAPI Hook_ChangeDisplaySettingsW(DEVMODEW* mode, DWORD) {
    Log(L"Display: blocked a display mode change (%s)", mode ? L"set mode" : L"restore");
    return DISP_CHANGE_SUCCESSFUL;
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

}  // namespace

void InitDisplay(HMODULE exe) {
    wchar_t mode[32];
    SettingString(L"Display", L"Mode", L"FGUYMOD_MODE", L"default", mode, 32);
    if (_wcsicmp(mode, L"fullscreen") == 0) g_mode = Mode::Fullscreen;
    else if (_wcsicmp(mode, L"windowed") == 0) g_mode = Mode::Windowed;
    else if (_wcsicmp(mode, L"borderless") == 0) g_mode = Mode::Borderless;
    if (SettingInt(L"Display", L"_", L"FGUYMOD_WINDOWED", 0)) g_mode = Mode::Windowed;  // test scripts

    g_width = SettingInt(L"Display", L"Width", nullptr, 0);
    g_height = SettingInt(L"Display", L"Height", nullptr, 0);
    wchar_t res[32];
    SettingString(L"Display", L"_", L"FGUYMOD_RES", L"", res, 32);
    if (res[0]) swscanf_s(res, L"%dx%d", &g_width, &g_height);
    g_background = SettingInt(L"Display", L"RunInBackground", L"FGUYMOD_BACKGROUND", 0) != 0;
    g_padMode = SettingInt(L"Debug", L"PadMode", L"FGUYMOD_PADMODE", 0) != 0;

    if (g_mode == Mode::Windowed || g_mode == Mode::Borderless) MakeDpiAware();

    if (g_mode == Mode::Borderless) {
        MONITORINFO mi = {sizeof(mi)};
        GetMonitorInfoW(MonitorFromPoint({0, 0}, MONITOR_DEFAULTTOPRIMARY), &mi);
        g_monitor = mi.rcMonitor;
        if (g_width <= 0 || g_height <= 0) {
            g_width = g_monitor.right - g_monitor.left;
            g_height = g_monitor.bottom - g_monitor.top;
        }
    }
    Log(L"Display: mode %s, render size %s%dx%d, run in background %s", ModeName(g_mode),
        g_width > 0 ? L"" : L"(game setting) ", g_width, g_height, g_background ? L"on" : L"off");

    HookSpec regHooks[] = {
        {"advapi32.dll", "RegQueryValueExW", (void*)Hook_RegQueryValueExW, (void**)&Real_RegQueryValueExW},
    };
    InstallHooks(exe, regHooks);

    HookSpec createHook[] = {
        {"user32.dll", "CreateWindowExW", (void*)Hook_CreateWindowExW, (void**)&Real_CreateWindowExW},
    };
    InstallHooks(exe, createHook);

    if (g_background) {
        HookSpec clipHook[] = {
            {"user32.dll", "ClipCursor", (void*)Hook_ClipCursor, (void**)&Real_ClipCursor},
        };
        InstallHooks(exe, clipHook);
    }

    if (g_mode == Mode::Borderless) {
        HookSpec windowHooks[] = {
            {"user32.dll", "SetWindowLongW", (void*)Hook_SetWindowLongW, (void**)&Real_SetWindowLongW},
            {"user32.dll", "SetWindowPos", (void*)Hook_SetWindowPos, (void**)&Real_SetWindowPos},
            {"user32.dll", "AdjustWindowRect", (void*)Hook_AdjustWindowRect, (void**)&Real_AdjustWindowRect},
            {"user32.dll", "ChangeDisplaySettingsW", (void*)Hook_ChangeDisplaySettingsW, (void**)&Real_ChangeDisplaySettingsW},
        };
        InstallHooks(exe, windowHooks);
    }
}
