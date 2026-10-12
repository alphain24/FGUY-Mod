// A notice over the game window, for online matches: "Waiting for Guest... (7 s)", "Guest left the match",
// "The match went out of sync". While the match waits for someone's input the game itself draws nothing (its
// frame is waiting), so this is a window of our own: on its own thread, always on top of the game, never taking
// the focus or the mouse, a dark box with the text near the top of the game's window.

#include "common.h"

#include <cwchar>

#pragma comment(lib, "gdi32.lib")

namespace {

const UINT kUpdateMsg = WM_APP + 1;
HWND g_box = nullptr;
HANDLE g_ready = nullptr;
CRITICAL_SECTION g_lock;
bool g_init = false;
wchar_t g_text[200] = L"";
bool g_show = false;
ULONGLONG g_hideAt = 0;   // a notice shown for a while (0: until hidden)

LRESULT CALLBACK BoxProc(HWND wnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_PAINT) {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(wnd, &ps);
        RECT r;
        GetClientRect(wnd, &r);
        HBRUSH bg = CreateSolidBrush(RGB(0x13, 0x21, 0x3C));
        FillRect(dc, &r, bg);
        DeleteObject(bg);
        HBRUSH edge = CreateSolidBrush(RGB(0xF5, 0xD3, 0x3A));
        FrameRect(dc, &r, edge);
        DeleteObject(edge);
        HFONT font = CreateFontW(-22, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                                 CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
        HGDIOBJ old = SelectObject(dc, font);
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, RGB(255, 255, 255));
        wchar_t text[200];
        EnterCriticalSection(&g_lock);
        wcscpy_s(text, g_text);
        LeaveCriticalSection(&g_lock);
        DrawTextW(dc, text, -1, &r, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
        SelectObject(dc, old);
        DeleteObject(font);
        EndPaint(wnd, &ps);
        return 0;
    }
    if (msg == kUpdateMsg || msg == WM_TIMER) {
        EnterCriticalSection(&g_lock);
        if (g_hideAt && GetTickCount64() >= g_hideAt) { g_show = false; g_hideAt = 0; }
        bool show = g_show;
        LeaveCriticalSection(&g_lock);
        RECT game;
        if (show && DisplayWindowRect(&game)) {
            int w = 560, h = 64, gw = game.right - game.left;
            int x = game.left + (gw - w) / 2, y = game.top + (game.bottom - game.top) / 5;
            SetWindowPos(wnd, HWND_TOPMOST, x, y, w, h, SWP_NOACTIVATE | SWP_SHOWWINDOW);
            InvalidateRect(wnd, nullptr, FALSE);
        } else {
            ShowWindow(wnd, SW_HIDE);
        }
        return 0;
    }
    if (msg == WM_NCHITTEST) return HTTRANSPARENT;
    return DefWindowProcW(wnd, msg, wp, lp);
}

DWORD WINAPI BoxThread(LPVOID) {
    WNDCLASSW wc = {};
    wc.lpfnWndProc = BoxProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"FGUYModNotice";
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    RegisterClassW(&wc);
    g_box = CreateWindowExW(WS_EX_LAYERED | WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_TRANSPARENT,
                            wc.lpszClassName, L"", WS_POPUP, 0, 0, 10, 10, nullptr, nullptr, wc.hInstance, nullptr);
    if (g_box) {
        SetLayeredWindowAttributes(g_box, 0, 235, LWA_ALPHA);
        SetTimer(g_box, 1, 250, nullptr);   // (timed notices go away by themselves; it follows the game window)
    }
    SetEvent(g_ready);
    MSG m;
    while (GetMessageW(&m, nullptr, 0, 0) > 0) {
        TranslateMessage(&m);
        DispatchMessageW(&m);
    }
    return 0;
}

void Update() {
    if (!g_init) {
        InitializeCriticalSection(&g_lock);
        g_ready = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        g_init = true;
        CloseHandle(CreateThread(nullptr, 0, BoxThread, nullptr, 0, nullptr));
        WaitForSingleObject(g_ready, 2000);
    }
    if (g_box) PostMessageW(g_box, kUpdateMsg, 0, 0);
}

}  // namespace

// Show `text` (until OverlayHide, or for `ms` milliseconds if given). Any thread.
void OverlayShow(const wchar_t* text, DWORD ms) {
    if (!g_init) Update();   // (creates the lock)
    EnterCriticalSection(&g_lock);
    bool same = g_show && wcscmp(g_text, text) == 0 && (ms ? g_hideAt != 0 : g_hideAt == 0);
    if (!g_show || ms) Log(L"Notice: %s", text);   // (not every second of a countdown)
    wcscpy_s(g_text, text);
    g_show = true;
    g_hideAt = ms ? GetTickCount64() + ms : 0;
    LeaveCriticalSection(&g_lock);
    if (!same) Update();
}

void OverlayHide() {
    if (!g_init) return;
    EnterCriticalSection(&g_lock);
    bool was = g_show && !g_hideAt;   // (a timed notice stays its time)
    if (was) g_show = false;
    LeaveCriticalSection(&g_lock);
    if (was) Update();
}
