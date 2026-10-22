// Direct3D 9: resolution list, frame-rate setting, and a startup crash fix.
//
// * Resolutions: the game lists a resolution only if both Windows (EnumDisplaySettingsW) and
//   Direct3D (EnumAdapterModes) report it at exactly 60 or 75 Hz, and stops after 64 entries. Modern
//   monitors break both rules (high refresh rates, long mode lists), so 4K etc. went missing. We give
//   both enumerations the same clean list: every 32-bit resolution the monitor supports, once each,
//   reported at 60 Hz. For real fullscreen we then pick a refresh rate the monitor really supports.
// * Frame rate ([Display] FrameRate = vsync | 30 | 60 | 120 | 144 | 240 | uncapped): presents
//   without vsync and paces frames ourselves. "vsync" in windowed/borderless waits for the desktop
//   compositor (no tearing); in exclusive fullscreen it keeps the game's own vsync. "uncapped" still
//   stops at 500 fps: some of the game's timers (e.g. the legal screen at boot) never finish when
//   frames take under a millisecond.
// * Crash fix: the game sometimes fails to create its device at startup and then crashes using it
//   (FGUY.exe +0x876e9). We retry CreateDevice - for up to 90 s, also through a fresh Direct3D object (right
//   after another copy of the game closed, the driver can refuse new devices for a while, and the game's own
//   Direct3D object can keep failing after a new one would work).

#include "common.h"

#include <d3d9.h>
#include <intrin.h>
#include <mmsystem.h>

#include <algorithm>
#include <vector>

#pragma comment(lib, "winmm.lib")

namespace {

// ---- frame rate choices (also the order of the in-game list, see ui.cpp) ------------------------
struct FrameRateChoice {
    const wchar_t* name;
    int fps;  // 0 = vsync, -1 = uncapped
};
const FrameRateChoice kFrameRates[] = {
    {L"vsync", 0}, {L"30", 30}, {L"60", 60}, {L"120", 120}, {L"144", 144}, {L"240", 240}, {L"uncapped", -1},
};
const int kFrameRateCount = sizeof(kFrameRates) / sizeof(kFrameRates[0]);
int g_frameRate = 0;           // index into kFrameRates
volatile bool g_quitRequested = false;
bool g_fullscreen = false;     // current device is exclusive fullscreen

// ---- curated resolution list --------------------------------------------------------------------
struct Res {
    DWORD w, h;
};
std::vector<Res> g_modes;
decltype(&EnumDisplaySettingsW) Real_EnumDisplaySettingsW;

void BuildModeList() {
    if (!g_modes.empty()) return;
    DEVMODEW dm = {};
    dm.dmSize = sizeof(dm);
    for (DWORD i = 0; Real_EnumDisplaySettingsW(nullptr, i, &dm); ++i) {
        if (dm.dmBitsPerPel != 32 || dm.dmPelsWidth < 1024 || dm.dmPelsHeight < 720) continue;
        bool dup = false;
        for (auto& r : g_modes) dup |= r.w == dm.dmPelsWidth && r.h == dm.dmPelsHeight;
        if (!dup) g_modes.push_back({dm.dmPelsWidth, dm.dmPelsHeight});
    }
    std::sort(g_modes.begin(), g_modes.end(),
              [](const Res& a, const Res& b) { return a.w != b.w ? a.w < b.w : a.h < b.h; });
    if (g_modes.size() > 60) g_modes.erase(g_modes.begin(), g_modes.end() - 60);  // keep the largest
    wchar_t list[2048] = L"";
    for (auto& r : g_modes) {
        wchar_t one[24];
        swprintf_s(one, L" %lux%lu", r.w, r.h);
        wcscat_s(list, one);
    }
    Log(L"D3D: resolutions offered to the game:%s", list);
}

// The game's list of sizes for Options > Screen (and for checking the saved one): 0x4c2b40(out, max, &count,
// windowed, filtered) keeps only the monitor's sizes that are also on a built-in list of nine, up to 1920 x 1080 -
// no 2560 x 1440, no 3840 x 2160. Ours: every size the monitor has (from 1024 x 720 up), the largest if there are
// more than fit, and windowed only those that fit on the screen. Entries: width, height, widescreen.
void __cdecl ValidDisplayModes(void* out, unsigned max, unsigned* count, char windowed, char) {
    BuildModeList();
    std::vector<Res> fit;
    int sw = GetSystemMetrics(SM_CXSCREEN), sh = GetSystemMetrics(SM_CYSCREEN);
    for (auto& r : g_modes)
        if (!windowed || ((int)r.w <= sw && (int)r.h <= sh)) fit.push_back(r);
    size_t first = fit.size() > max ? fit.size() - max : 0;
    unsigned n = 0;
    for (size_t i = first; i < fit.size(); ++i, ++n) {
        UINT* e = reinterpret_cast<UINT*>(static_cast<char*>(out) + n * 12);
        e[0] = fit[i].w;
        e[1] = fit[i].h;
        e[2] = fit[i].w * 10 >= fit[i].h * 14 ? 1 : 0;
    }
    *count = n;
}

bool PatchCallTo(uintptr_t at, uintptr_t expected, void* to) {
    auto b = reinterpret_cast<unsigned char*>(at);
    if (IsBadReadPtr(b, 5) || b[0] != 0xE8 || at + 5 + *reinterpret_cast<int*>(b + 1) != expected) return false;
    DWORD old;
    VirtualProtect(b, 5, PAGE_EXECUTE_READWRITE, &old);
    *reinterpret_cast<int*>(b + 1) = (int)(reinterpret_cast<uintptr_t>(to) - (at + 5));
    VirtualProtect(b, 5, old, &old);
    FlushInstructionCache(GetCurrentProcess(), b, 5);
    return true;
}

BOOL WINAPI Hook_EnumDisplaySettingsW(LPCWSTR device, DWORD mode, DEVMODEW* dm) {
    if (device || mode == ENUM_CURRENT_SETTINGS || mode == ENUM_REGISTRY_SETTINGS || !dm)
        return Real_EnumDisplaySettingsW(device, mode, dm);
    BuildModeList();
    if (mode >= g_modes.size()) return FALSE;
    WORD size = dm->dmSize;
    ZeroMemory(dm, size);
    dm->dmSize = size;
    dm->dmBitsPerPel = 32;
    dm->dmPelsWidth = g_modes[mode].w;
    dm->dmPelsHeight = g_modes[mode].h;
    dm->dmDisplayFrequency = 60;
    dm->dmFields = DM_BITSPERPEL | DM_PELSWIDTH | DM_PELSHEIGHT | DM_DISPLAYFREQUENCY;
    return TRUE;
}

// ---- IDirect3D9 ---------------------------------------------------------------------------------
using GetAdapterModeCount_t = UINT(STDMETHODCALLTYPE*)(IDirect3D9*, UINT, D3DFORMAT);
using EnumAdapterModes_t = HRESULT(STDMETHODCALLTYPE*)(IDirect3D9*, UINT, D3DFORMAT, UINT, D3DDISPLAYMODE*);
using CreateDevice_t = HRESULT(STDMETHODCALLTYPE*)(IDirect3D9*, UINT, D3DDEVTYPE, HWND, DWORD,
                                                   D3DPRESENT_PARAMETERS*, IDirect3DDevice9**);
using Direct3DCreate9_t = IDirect3D9*(WINAPI*)(UINT);
Direct3DCreate9_t Real_Direct3DCreate9;
GetAdapterModeCount_t Real_GetAdapterModeCount;
EnumAdapterModes_t Real_EnumAdapterModes;
CreateDevice_t Real_CreateDevice;
IDirect3D9* g_d3d = nullptr;
UINT g_adapter = D3DADAPTER_DEFAULT;   // the adapter the game's device is on

UINT STDMETHODCALLTYPE Hook_GetAdapterModeCount(IDirect3D9* self, UINT adapter, D3DFORMAT format) {
    if (adapter != D3DADAPTER_DEFAULT || format != D3DFMT_X8R8G8B8)
        return Real_GetAdapterModeCount(self, adapter, format);
    BuildModeList();
    return (UINT)g_modes.size();
}

HRESULT STDMETHODCALLTYPE Hook_EnumAdapterModes(IDirect3D9* self, UINT adapter, D3DFORMAT format, UINT mode,
                                                D3DDISPLAYMODE* out) {
    if (adapter != D3DADAPTER_DEFAULT || format != D3DFMT_X8R8G8B8)
        return Real_EnumAdapterModes(self, adapter, format, mode, out);
    BuildModeList();
    if (mode >= g_modes.size() || !out) return D3DERR_INVALIDCALL;
    out->Width = g_modes[mode].w;
    out->Height = g_modes[mode].h;
    out->RefreshRate = 60;
    out->Format = D3DFMT_X8R8G8B8;
    return D3D_OK;
}

// The highest refresh rate the monitor really has at this size.
UINT RealRefreshRate(UINT w, UINT h) {
    if (!g_d3d) return 0;
    UINT best = 0, count = Real_GetAdapterModeCount(g_d3d, g_adapter, D3DFMT_X8R8G8B8);
    for (UINT i = 0; i < count; ++i) {
        D3DDISPLAYMODE m;
        if (FAILED(Real_EnumAdapterModes(g_d3d, g_adapter, D3DFMT_X8R8G8B8, i, &m))) continue;
        if (m.Width != w || m.Height != h) continue;
        if (m.RefreshRate > best) best = m.RefreshRate;
    }
    return best;
}

// Adjust the game's present parameters for our frame-rate setting and real refresh rates.
void FixPresentParameters(D3DPRESENT_PARAMETERS* pp) {
    g_fullscreen = !pp->Windowed;
    if (pp->Windowed) {
        // Never vsync in windowed modes: the compositor prevents tearing, and we pace frames ourselves.
        pp->PresentationInterval = D3DPRESENT_INTERVAL_IMMEDIATE;
        pp->FullScreen_RefreshRateInHz = 0;
    } else {
        pp->FullScreen_RefreshRateInHz = RealRefreshRate(pp->BackBufferWidth, pp->BackBufferHeight);
        if (kFrameRates[g_frameRate].fps != 0) pp->PresentationInterval = D3DPRESENT_INTERVAL_IMMEDIATE;
    }
}

// ---- IDirect3DDevice9: Reset / Present ------------------------------------------------------------
using Reset_t = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, D3DPRESENT_PARAMETERS*);
using Present_t = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, const RECT*, const RECT*, HWND, const RGNDATA*);
Reset_t Real_Reset;
Present_t Real_Present;
using CreateTexture_t = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, UINT, UINT, UINT, DWORD, D3DFORMAT, D3DPOOL,
                                                    IDirect3DTexture9**, HANDLE*);
CreateTexture_t Real_CreateTexture;

// Only watches: a texture the game can't get goes into the log (the game doesn't check - it crashes after).
HRESULT STDMETHODCALLTYPE Hook_CreateTexture(IDirect3DDevice9* self, UINT w, UINT h, UINT levels, DWORD usage,
                                             D3DFORMAT format, D3DPOOL pool, IDirect3DTexture9** out, HANDLE* share) {
    HRESULT hr = Real_CreateTexture(self, w, h, levels, usage, format, pool, out, share);
    if (FAILED(hr)) {
        static volatile LONG count = 0;
        if (InterlockedIncrement(&count) <= 10)
            Log(L"D3D: CreateTexture %ux%u levels %u usage %#lx format %u pool %u -> %#lx (called from %p; game's "
                L"screen %dx%d)", w, h, levels, usage, (unsigned)format, (unsigned)pool, hr, _ReturnAddress(),
                *reinterpret_cast<int*>(0xaf2fbc), *reinterpret_cast<int*>(0xaf2fb8));
    }
    return hr;
}

// Why exclusive fullscreen might be refused: what was asked, and the modes the device's adapter has at that size.
void LogFullscreenTrouble(IDirect3DDevice9* dev, const D3DPRESENT_PARAMETERS& pp) {
    D3DDEVICE_CREATION_PARAMETERS cp = {};
    dev->GetCreationParameters(&cp);
    Log(L"D3D:   fullscreen %ux%u format %d, %u back buffers, multisample %d, swap %d, window %p, adapter %u",
        pp.BackBufferWidth, pp.BackBufferHeight, pp.BackBufferFormat, pp.BackBufferCount, pp.MultiSampleType,
        pp.SwapEffect, pp.hDeviceWindow, cp.AdapterOrdinal);
    IDirect3D9* d3d = nullptr;
    if (FAILED(dev->GetDirect3D(&d3d)) || !d3d) return;
    UINT count = Real_GetAdapterModeCount(d3d, cp.AdapterOrdinal, D3DFMT_X8R8G8B8);
    wchar_t line[1024];
    int len = swprintf_s(line, L"D3D:   adapter %u has %u modes; at %ux%u:", cp.AdapterOrdinal, count,
                         pp.BackBufferWidth, pp.BackBufferHeight);
    UINT maxW = 0, maxH = 0;
    for (UINT i = 0; i < count && len < 960; ++i) {
        D3DDISPLAYMODE m;
        if (FAILED(Real_EnumAdapterModes(d3d, cp.AdapterOrdinal, D3DFMT_X8R8G8B8, i, &m))) continue;
        if (m.Width * m.Height > maxW * maxH) { maxW = m.Width; maxH = m.Height; }
        if (m.Width == pp.BackBufferWidth && m.Height == pp.BackBufferHeight)
            len += swprintf_s(line + len, 1024 - len, L" %uHz", m.RefreshRate);
    }
    Log(L"%s (largest mode %ux%u)", line, maxW, maxH);
    d3d->Release();
}

HRESULT STDMETHODCALLTYPE Hook_Reset(IDirect3DDevice9* self, D3DPRESENT_PARAMETERS* pp) {
    D3DPRESENT_PARAMETERS original = *pp, mine = *pp;
    FixPresentParameters(&mine);
    HRESULT hr = Real_Reset(self, &mine);
    Log(L"D3D: Reset %ux%u %s interval %#x refresh %u -> %#lx", mine.BackBufferWidth, mine.BackBufferHeight,
        mine.Windowed ? L"windowed" : L"fullscreen", mine.PresentationInterval, mine.FullScreen_RefreshRateInHz, hr);
    if (FAILED(hr)) {
        *pp = original;  // try exactly what the game asked for
        hr = Real_Reset(self, pp);
        Log(L"D3D: Reset with the game's own parameters (interval %#x refresh %u) -> %#lx",
            original.PresentationInterval, original.FullScreen_RefreshRateInHz, hr);
    } else {
        *pp = mine;
    }
    if (FAILED(hr) && !original.Windowed) {
        LogFullscreenTrouble(self, original);
        D3DPRESENT_PARAMETERS win = original;
        win.Windowed = TRUE;
        win.FullScreen_RefreshRateInHz = 0;
        win.PresentationInterval = D3DPRESENT_INTERVAL_IMMEDIATE;
        DisplayFullscreenFailed();   // borderless from here on: the window covers the screen, no title bar
        hr = Real_Reset(self, &win);
        Log(L"D3D: fullscreen refused - borderless at %ux%u instead -> %#lx", win.BackBufferWidth,
            win.BackBufferHeight, hr);
        if (SUCCEEDED(hr)) {
            *pp = win;
            g_fullscreen = false;
        }
    }
    DisplayAfterReset();
    return hr;
}

LARGE_INTEGER g_freq, g_lastFrame, g_statStart;
int g_statFrames = 0;
using DwmFlush_t = HRESULT(WINAPI*)();
DwmFlush_t g_dwmFlush = nullptr;

volatile ULONGLONG g_hudShownAt = 0;   // the level's HUD last said it shows (0: hidden)

void PaceFrame() {
    // Netplay runs the game on a fixed 1/60 s clock, so real time must match: always 60 fps - except while an
    // online session boots hidden (title, menus, level load: nobody watches), which runs as fast as the PCs
    // can (still in lockstep), so the match is up sooner.
    // A match that runs free (no lockstep) runs at the player's own frame rate, as offline.
    if (NetplayEnabled() && !NetplayFreeRun()) {   // online: the ticks keep real time themselves (netplay.cpp PaceTick)
        QueryPerformanceCounter(&g_lastFrame);
        return;
    }
    int fps = kFrameRates[g_frameRate].fps;
    if (fps < 0) fps = 500;  // "uncapped": see the top of this file
    // Menus and cutscenes at 60 at most: some of their scripts count frames (above 60 a menu's slide-in stopped
    // halfway - Story Mode's, shifted up, its buttons' help gone - and cutscenes went wrong). The chosen rate only
    // while the game is played: the level's HUD up and showing (the HUD says so, VideoHudShown; it hides itself for
    // cutscenes, and there's none in the menus).
    bool playing = GetTickCount64() - g_hudShownAt < 400;
    if (!playing && (fps <= 0 || fps > 60)) fps = 60;
    if (OnlineStartingMatch() && (fps <= 0 || fps > 60)) fps = 60;   // only the loading screen: leave the GPU to
                                                                    // the match's game, booting meanwhile
    if (fps == 0 && !g_fullscreen && g_dwmFlush) {
        g_dwmFlush();  // vsync in windowed modes: wait for the compositor's next frame
    } else if (fps > 0) {
        LARGE_INTEGER now;
        LONGLONG frame = g_freq.QuadPart / fps;
        QueryPerformanceCounter(&now);
        LONGLONG target = g_lastFrame.QuadPart + frame;
        if (now.QuadPart < target) {
            LONGLONG ms = (target - now.QuadPart) * 1000 / g_freq.QuadPart;
            if (ms > 1) Sleep((DWORD)(ms - 1));
            do QueryPerformanceCounter(&now); while (now.QuadPart < target);
            g_lastFrame.QuadPart = target;
        } else {
            g_lastFrame = now;  // running slower than the cap
        }
        return;
    }
    QueryPerformanceCounter(&g_lastFrame);
}

// Ends the game for the mod (test tools, an online game handing over to its session). ExitProcess runs the
// game's exit-time cleanup, which crashes (FGUY.exe 0x6c2f8c: a destructor using an allocator that's
// already gone) - and a crash makes Windows report it and leaves the graphics driver unsettled for the next
// copy. So once we're exiting, a crash there just ends the process on the spot.
LONG CALLBACK ExitCrashHandler(EXCEPTION_POINTERS* e) {
    if (e && e->ExceptionRecord && e->ExceptionRecord->ExceptionCode == EXCEPTION_ACCESS_VIOLATION)
        TerminateProcess(GetCurrentProcess(), 0);
    return EXCEPTION_CONTINUE_SEARCH;
}
}  // namespace

// The level's HUD, 30 times a second (pause.as2js FGUYPausePoll): is it showing? (the game hides it for cutscenes)
void VideoHudShown(bool shown) { g_hudShownAt = shown ? GetTickCount64() : 0; }

void QuitGame() {
    static volatile LONG once = 0;
    if (InterlockedExchange(&once, 1)) {   // someone's already on it
        Sleep(INFINITE);
    }
    AddVectoredExceptionHandler(1, ExitCrashHandler);
    ExitProcess(0);
}

namespace {

// Test tools' screenshot (VideoRequestShot): the frame about to be shown, copied out of the back buffer and
// written as a 24-bit .bmp at half size. (Reading the window from outside - PrintWindow - could leave the game
// not drawing any more.)
CRITICAL_SECTION g_shotLock;
bool g_shotInit = false;
wchar_t g_shotPath[1024] = L"";

void SaveShot(IDirect3DDevice9* dev, const wchar_t* path) {
    IDirect3DSurface9 *back = nullptr, *copy = nullptr;
    D3DSURFACE_DESC d;
    bool ok = false;
    if (SUCCEEDED(dev->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &back)) && SUCCEEDED(back->GetDesc(&d)) &&
        (d.Format == D3DFMT_X8R8G8B8 || d.Format == D3DFMT_A8R8G8B8) &&
        SUCCEEDED(dev->CreateOffscreenPlainSurface(d.Width, d.Height, d.Format, D3DPOOL_SYSTEMMEM, &copy, nullptr)) &&
        SUCCEEDED(dev->GetRenderTargetData(back, copy))) {
        D3DLOCKED_RECT lr;
        if (SUCCEEDED(copy->LockRect(&lr, nullptr, D3DLOCK_READONLY))) {
            int w = (int)d.Width / 2, h = (int)d.Height / 2, row = (w * 3 + 3) & ~3;
            std::vector<unsigned char> img(54 + (size_t)row * h, 0);
            unsigned char* hd = img.data();
            hd[0] = 'B'; hd[1] = 'M';
            *reinterpret_cast<DWORD*>(hd + 2) = (DWORD)img.size();
            *reinterpret_cast<DWORD*>(hd + 10) = 54;
            *reinterpret_cast<DWORD*>(hd + 14) = 40;
            *reinterpret_cast<LONG*>(hd + 18) = w;
            *reinterpret_cast<LONG*>(hd + 22) = h;
            *reinterpret_cast<WORD*>(hd + 26) = 1;
            *reinterpret_cast<WORD*>(hd + 28) = 24;
            for (int y = 0; y < h; ++y) {
                const unsigned char* src = static_cast<const unsigned char*>(lr.pBits) + (size_t)(y * 2) * lr.Pitch;
                unsigned char* dst = hd + 54 + (size_t)(h - 1 - y) * row;
                for (int x = 0; x < w; ++x) memcpy(dst + x * 3, src + x * 8, 3);   // B, G, R of every other pixel
            }
            copy->UnlockRect();
            HANDLE f = CreateFileW(path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
            DWORD put = 0;
            if (f != INVALID_HANDLE_VALUE) {
                ok = WriteFile(f, img.data(), (DWORD)img.size(), &put, nullptr) && put == img.size();
                CloseHandle(f);
            }
        }
    }
    if (copy) copy->Release();
    if (back) back->Release();
    Log(L"D3D: screenshot %s %s", ok ? L"saved to" : L"failed:", path);
}

HRESULT STDMETHODCALLTYPE Hook_Present(IDirect3DDevice9* self, const RECT* src, const RECT* dst, HWND wnd,
                                       const RGNDATA* dirty) {
    if (g_shotInit && g_shotPath[0]) {
        wchar_t path[1024];
        EnterCriticalSection(&g_shotLock);
        wcscpy_s(path, g_shotPath);
        g_shotPath[0] = 0;
        LeaveCriticalSection(&g_shotLock);
        if (path[0]) SaveShot(self, path);
    }
    HRESULT hr = Real_Present(self, src, dst, wnd, dirty);
    if (g_quitRequested) {
        // Exiting from the render thread between frames lets Direct3D and the driver clean up in this
        // process. Killing the process instead can leave the driver unable to create D3D9 devices for a
        // minute or two on some PCs.
        Log(L"D3D: quit requested - exiting");
        QuitGame();
    }
    PaceFrame();
    DisplayCheckStyle();
    ModsFrame();   // the mods' plugins (modloader.cpp)
    CosmeticsFrame(self);   // skins (cosmetics.cpp)
    NetplayOnFrame();
    OnlineMenuFrame();
    static unsigned frame = 0;
    ResearchOnFrame(++frame);
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    ++g_statFrames;
    if (now.QuadPart - g_statStart.QuadPart >= g_freq.QuadPart * 10) {
        Log(L"D3D: %.1f fps (frame rate setting: %s)",
            g_statFrames * (double)g_freq.QuadPart / (now.QuadPart - g_statStart.QuadPart), kFrameRates[g_frameRate].name);
        g_statStart = now;
        g_statFrames = 0;
    }
    return hr;
}

void* PatchVtable(void* object, int slot, void* hook) {
    void** vtable = *reinterpret_cast<void***>(object);
    if (vtable[slot] == hook) return nullptr;
    DWORD old;
    VirtualProtect(&vtable[slot], sizeof(void*), PAGE_READWRITE, &old);
    void* original = vtable[slot];
    vtable[slot] = hook;
    VirtualProtect(&vtable[slot], sizeof(void*), old, &old);
    return original;
}

IDirect3D9* CreateD3D(UINT sdk);

HRESULT STDMETHODCALLTYPE Hook_CreateDevice(IDirect3D9* self, UINT adapter, D3DDEVTYPE type, HWND focus,
                                            DWORD flags, D3DPRESENT_PARAMETERS* pp, IDirect3DDevice9** out) {
    D3DPRESENT_PARAMETERS original = *pp;  // CreateDevice may modify it; retry with what the game asked
    HRESULT hr = E_FAIL;
    ULONGLONG giveUp = GetTickCount64() + 90000;
    for (int attempt = 1; attempt <= 20 || GetTickCount64() < giveUp; ++attempt) {
        *pp = original;
        FixPresentParameters(pp);
        hr = Real_CreateDevice(self, adapter, type, focus, flags, pp, out);
        if (attempt <= 20 || attempt % 10 == 0 || SUCCEEDED(hr))
            Log(L"D3D: CreateDevice %ux%u %s -> %#lx%s", original.BackBufferWidth, original.BackBufferHeight,
                original.Windowed ? L"windowed" : L"fullscreen", hr, attempt > 1 ? L" (retry)" : L"");
        if (SUCCEEDED(hr)) break;
        if (attempt >= 4 && attempt % 2 == 0) {   // a fresh Direct3D object
            if (IDirect3D9* fresh = CreateD3D(D3D_SDK_VERSION)) {
                *pp = original;
                FixPresentParameters(pp);
                hr = Real_CreateDevice(fresh, adapter, type, focus, flags, pp, out);
                if (SUCCEEDED(hr)) {
                    Log(L"D3D: CreateDevice through a fresh Direct3D object -> 0");
                    break;
                }
                fresh->Release();
            }
        }
        if (attempt == 1) {
            RECT rc = {};
            GetClientRect(pp->hDeviceWindow ? pp->hDeviceWindow : focus, &rc);
            Log(L"D3D:   format %d, %u back buffers, multisample %d/%lu, swap %d, window %p (client %ldx%ld, "
                L"visible %d, iconic %d), focus %p, flags %#lx, depth %d/%d, ppflags %#lx",
                pp->BackBufferFormat, pp->BackBufferCount, pp->MultiSampleType, pp->MultiSampleQuality,
                pp->SwapEffect, pp->hDeviceWindow, rc.right, rc.bottom,
                IsWindowVisible(pp->hDeviceWindow ? pp->hDeviceWindow : focus),
                IsIconic(pp->hDeviceWindow ? pp->hDeviceWindow : focus), focus, flags,
                pp->EnableAutoDepthStencil, pp->AutoDepthStencilFormat, pp->Flags);
        }
        if (attempt == 1) {
            // List the adapters: PCs with virtual displays (Parsec, virtual display drivers, remote
            // desktop tools) can put an adapter without 3D acceleration first.
            HMONITOR wantMon = MonitorFromWindow(focus, MONITOR_DEFAULTTOPRIMARY);
            UINT count = self->GetAdapterCount(), better = adapter;
            for (UINT a = 0; a < count; ++a) {
                D3DADAPTER_IDENTIFIER9 id = {};
                self->GetAdapterIdentifier(a, 0, &id);
                HMONITOR mon = self->GetAdapterMonitor(a);
                Log(L"D3D:   adapter %u: %S%s%s", a, id.Description, a == adapter ? L"  <- game asked for this" : L"",
                    mon == wantMon ? L"  (shows the game window)" : L"");
                if (mon == wantMon && a != adapter) better = a;
            }
            if (better != adapter) {
                Log(L"D3D:   retrying on adapter %u", better);
                adapter = better;
            }
        }
        if (attempt == 3 && pp->MultiSampleType != D3DMULTISAMPLE_NONE) {
            // If anti-aliasing is what fails (e.g. not supported for this size/format), go without it.
            original.MultiSampleType = D3DMULTISAMPLE_NONE;
            original.MultiSampleQuality = 0;
            Log(L"D3D:   retrying without multisampling");
        }
        Sleep(attempt < 20 ? 250 : 1000);
    }
    if (SUCCEEDED(hr) && out && *out) {
        g_adapter = adapter;
        if (void* o = PatchVtable(*out, 16, (void*)Hook_Reset)) Real_Reset = (Reset_t)o;
        if (void* o = PatchVtable(*out, 17, (void*)Hook_Present)) Real_Present = (Present_t)o;
        if (void* o = PatchVtable(*out, 23, (void*)Hook_CreateTexture)) Real_CreateTexture = (CreateTexture_t)o;
    }
    return hr;
}

// Direct3D 9 on Direct3D 12: Microsoft's mapping layer, part of Windows 10 / 11 (d3d9.dll's
// Direct3DCreate9On12 - nothing extra installed). The game keeps talking Direct3D 9; underneath it's Direct3D 12
// with modern presentation. Off by default: on the user's PC it showed nothing until the first reset.
// [Display] Renderer=d3d12 (or FGUYMOD_RENDERER=d3d12) turns it on; a Windows without the layer stays on 9.
struct D3D9On12Args {
    BOOL Enable9On12;
    IUnknown* pD3D12Device;
    IUnknown* ppD3D12Queues[2];
    UINT NumQueues;
    UINT NodeMask;
};
using Create9On12_t = IDirect3D9*(WINAPI*)(UINT sdk, D3D9On12Args* args, UINT count);
bool g_on12 = false;

// DXVK (FGUYMod\dxvk\dxvk_d3d9.dll - DXVK 3.1.1's own 32-bit d3d9.dll, zlib license): Direct3D 9 on Vulkan,
// the translation Valve's Proton uses. It doesn't go through Windows' Direct3D 9 at all. Its log and settings
// file (dxvk.conf) are in that folder. The default ([Display] Renderer=dxvk); a PC without a GPU it can use (it
// needs Vulkan 1.3) gets plain Direct3D 9 by itself, and Renderer=d3d9 always does.
// Some PCs have a GPU whose driver installed its Vulkan files but didn't register them (seen: an NVIDIA laptop
// GPU with only "Vulkan SC" registered) - Vulkan programs then don't see that GPU at all. For this process only,
// each display adapter's driver folder is checked for a 32-bit Vulkan driver file that isn't registered, and
// it's added (VK_ADD_DRIVER_FILES; nothing on the system changes).
void AddUnregisteredVulkanDrivers() {
    wchar_t have[8];
    if (GetEnvironmentVariableW(L"VK_ADD_DRIVER_FILES", have, 8) || GetEnvironmentVariableW(L"VK_DRIVER_FILES", have, 8)) return;
    HKEY cls;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"SYSTEM\\CurrentControlSet\\Control\\Class\\{4d36e968-e325-11ce-bfc1-08002be10318}", 0,
                      KEY_READ, &cls) != ERROR_SUCCESS)
        return;
    std::wstring add;
    wchar_t sub[64];
    for (DWORD i = 0; RegEnumKeyW(cls, i, sub, 64) == ERROR_SUCCESS; ++i) {
        HKEY k;
        if (RegOpenKeyExW(cls, sub, 0, KEY_READ, &k) != ERROR_SUCCESS) continue;
        wchar_t umd[MAX_PATH * 2] = L"", vk[MAX_PATH * 2] = L"";
        DWORD n = sizeof(umd), type = 0;
        bool hasUmd = RegQueryValueExW(k, L"UserModeDriverName", nullptr, &type, (BYTE*)umd, &n) == ERROR_SUCCESS && umd[0];
        n = sizeof(vk);
        bool hasVk = RegQueryValueExW(k, L"VulkanDriverNameWow", nullptr, &type, (BYTE*)vk, &n) == ERROR_SUCCESS && vk[0];
        RegCloseKey(k);
        if (!hasUmd || hasVk) continue;
        std::wstring dir(umd);
        dir = dir.substr(0, dir.find_last_of(L'\\'));
        WIN32_FIND_DATAW fd;
        HANDLE h = FindFirstFileW((dir + L"\\*vk32.json").c_str(), &fd);
        if (h == INVALID_HANDLE_VALUE) continue;
        do {
            if (wcsstr(fd.cFileName, L"vksc")) continue;   // (Vulkan SC: not for games)
            if (!add.empty()) add += L";";
            add += dir + L"\\" + fd.cFileName;
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    RegCloseKey(cls);
    if (add.empty()) return;
    SetEnvironmentVariableW(L"VK_ADD_DRIVER_FILES", add.c_str());
    Log(L"D3D: Vulkan driver(s) installed but not registered - added for the game: %s", add.c_str());
}

IDirect3D9* CreateDxvk(UINT sdk) {
    static HMODULE dll = nullptr;
    if (!dll) {
        wchar_t dir[MAX_PATH], path[MAX_PATH], conf[MAX_PATH], env[8];
        swprintf_s(dir, L"%s\\dxvk", g_modDir);
        swprintf_s(path, L"%s\\dxvk_d3d9.dll", dir);
        swprintf_s(conf, L"%s\\dxvk.conf", dir);
        if (!GetEnvironmentVariableW(L"DXVK_LOG_PATH", env, 8)) SetEnvironmentVariableW(L"DXVK_LOG_PATH", dir);
        if (!GetEnvironmentVariableW(L"DXVK_CONFIG_FILE", env, 8)) SetEnvironmentVariableW(L"DXVK_CONFIG_FILE", conf);
        AddUnregisteredVulkanDrivers();
        dll = LoadLibraryW(path);
        if (!dll) {
            Log(L"D3D: DXVK not found or not loadable (%s, error %lu)", path, GetLastError());
            return nullptr;
        }
    }
    auto create = reinterpret_cast<Direct3DCreate9_t>(GetProcAddress(dll, "Direct3DCreate9"));
    IDirect3D9* d3d = create ? create(sdk) : nullptr;
    if (d3d && d3d->GetAdapterCount() == 0) {   // no GPU it can use (too old a Vulkan): not this one
        d3d->Release();
        d3d = nullptr;
    }
    static bool logged = false;
    if (!logged) {
        logged = true;
        D3DADAPTER_IDENTIFIER9 id = {};
        if (d3d) d3d->GetAdapterIdentifier(D3DADAPTER_DEFAULT, 0, &id);
        if (d3d) Log(L"D3D: DXVK - Direct3D 9 on Vulkan, on %S", id.Description);
        else Log(L"D3D: DXVK found no GPU it can use (needs Vulkan 1.3: see FGUYMod\\dxvk\\FGUY_d3d9.log)");
    }
    return d3d;
}

IDirect3D9* CreateD3D(UINT sdk) {
    static int renderer = -1;   // 2: DXVK (Vulkan), 1: Direct3D 12 underneath, 0: plain Direct3D 9
    if (renderer < 0) {
        wchar_t r[16];
        SettingString(L"Display", L"Renderer", L"FGUYMOD_RENDERER", L"dxvk", r, 16);
        renderer = _wcsicmp(r, L"dxvk") == 0 ? 2 : _wcsicmp(r, L"d3d12") == 0 ? 1 : 0;
    }
    if (renderer == 2) {
        if (IDirect3D9* d3d = CreateDxvk(sdk)) return d3d;
        Log(L"D3D: DXVK isn't available here - plain Direct3D 9");
        renderer = 0;
    }
    if (renderer == 1) {
        auto create = reinterpret_cast<Create9On12_t>(GetProcAddress(GetModuleHandleW(L"d3d9.dll"), "Direct3DCreate9On12"));
        if (create) {
            D3D9On12Args args = {};
            args.Enable9On12 = TRUE;
            if (IDirect3D9* d3d = create(sdk, &args, 1)) {
                static bool logged = false;
                if (!logged) { logged = true; Log(L"D3D: Direct3D 9 on Direct3D 12 (Windows' own layer)"); }
                g_on12 = true;
                return d3d;
            }
        }
        Log(L"D3D: Direct3D 12 underneath isn't available here - plain Direct3D 9");
        renderer = 0;
    }
    return Real_Direct3DCreate9(sdk);
}

IDirect3D9* WINAPI Hook_Direct3DCreate9(UINT sdk) {
    IDirect3D9* d3d = CreateD3D(sdk);
    if (d3d) {
        g_d3d = d3d;
        if (void* o = PatchVtable(d3d, 6, (void*)Hook_GetAdapterModeCount)) Real_GetAdapterModeCount = (GetAdapterModeCount_t)o;
        if (void* o = PatchVtable(d3d, 7, (void*)Hook_EnumAdapterModes)) Real_EnumAdapterModes = (EnumAdapterModes_t)o;
        if (void* o = PatchVtable(d3d, 16, (void*)Hook_CreateDevice)) Real_CreateDevice = (CreateDevice_t)o;
    }
    return d3d;
}

void SaveFrameRate() {
    wchar_t path[MAX_PATH];
    swprintf_s(path, L"%s\\FGUYMod.ini", g_modDir);
    WritePrivateProfileStringW(L"Display", L"FrameRate", kFrameRates[g_frameRate].name, path);
}

}  // namespace

void VideoRequestQuit() { g_quitRequested = true; }
void VideoRequestShot(const wchar_t* path) {
    if (!g_shotInit) { InitializeCriticalSection(&g_shotLock); g_shotInit = true; }
    EnterCriticalSection(&g_shotLock);
    if (wcslen(path) < _countof(g_shotPath)) wcscpy_s(g_shotPath, path);
    LeaveCriticalSection(&g_shotLock);
}

// In-game Options > Screen > Frame Rate (see ui.cpp).
int VideoFrameRateIndex() { return g_frameRate; }
void VideoSetFrameRate(int index) {
    if (index < 0 || index >= kFrameRateCount || index == g_frameRate) return;
    g_frameRate = index;
    QueryPerformanceCounter(&g_lastFrame);
    SaveFrameRate();
    Log(L"D3D: frame rate set to %s%s", kFrameRates[index].name,
        g_fullscreen ? L" (vsync changes apply after a restart in fullscreen)" : L"");
}

void InitD3D9(HMODULE exe) {
    wchar_t fr[16];
    SettingString(L"Display", L"FrameRate", L"FGUYMOD_FPS", L"vsync", fr, 16);
    for (int i = 0; i < kFrameRateCount; ++i)
        if (_wcsicmp(fr, kFrameRates[i].name) == 0) g_frameRate = i;
    Log(L"D3D: frame rate setting %s", kFrameRates[g_frameRate].name);

    QueryPerformanceFrequency(&g_freq);
    QueryPerformanceCounter(&g_lastFrame);
    g_statStart = g_lastFrame;
    timeBeginPeriod(1);  // precise Sleep for the frame limiter
    const uintptr_t kModesFn = 0x4c2b40, kModesCalls[] = {0x5a577d, 0x4bba10};   // (Options > Screen; the saved size)
    int patched = 0;
    for (uintptr_t at : kModesCalls) patched += PatchCallTo(at, kModesFn, (void*)ValidDisplayModes);
    Log(L"D3D: %s", patched == 2 ? L"every size the monitor has is offered (not only the game's nine)"
                                 : L"size list code isn't what we expect - the game's own list");
    if (HMODULE dwm = LoadLibraryW(L"dwmapi.dll")) g_dwmFlush = (DwmFlush_t)GetProcAddress(dwm, "DwmFlush");

    HookSpec hooks[] = {
        {"d3d9.dll", "Direct3DCreate9", (void*)Hook_Direct3DCreate9, (void**)&Real_Direct3DCreate9},
        {"user32.dll", "EnumDisplaySettingsW", (void*)Hook_EnumDisplaySettingsW, (void**)&Real_EnumDisplaySettingsW},
    };
    InstallHooks(exe, hooks);
}
