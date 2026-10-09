// Direct3D 9: resolution list, frame-rate setting, and a startup crash fix.
//
// * Resolutions: the game lists a resolution only if both Windows (EnumDisplaySettingsW) and
//   Direct3D (EnumAdapterModes) report it at exactly 60 or 75 Hz, and stops after 64 entries. Modern
//   monitors break both rules (high refresh rates, long mode lists), so 4K etc. went missing. We give
//   both enumerations the same clean list: every 32-bit resolution the monitor supports, once each,
//   reported at 60 Hz. For real fullscreen we then pick a refresh rate the monitor really supports.
// * Frame rate ([Display] FrameRate = vsync | 30 | 60 | 120 | 144 | 240 | uncapped): presents
//   without vsync and paces frames ourselves. "vsync" in windowed/borderless waits for the desktop
//   compositor (no tearing); in exclusive fullscreen it keeps the game's own vsync.
// * Crash fix: the game sometimes fails to create its device at startup and then crashes using it
//   (FGUY.exe +0x876e9). We retry CreateDevice for a few seconds.

#include "common.h"

#include <d3d9.h>

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

// Best refresh rate the monitor really has at this size (prefers 60, else the highest).
UINT RealRefreshRate(UINT w, UINT h) {
    if (!g_d3d) return 0;
    UINT best = 0, count = Real_GetAdapterModeCount(g_d3d, D3DADAPTER_DEFAULT, D3DFMT_X8R8G8B8);
    for (UINT i = 0; i < count; ++i) {
        D3DDISPLAYMODE m;
        if (FAILED(Real_EnumAdapterModes(g_d3d, D3DADAPTER_DEFAULT, D3DFMT_X8R8G8B8, i, &m))) continue;
        if (m.Width != w || m.Height != h) continue;
        if (m.RefreshRate == 60) return 60;
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

HRESULT STDMETHODCALLTYPE Hook_Reset(IDirect3DDevice9* self, D3DPRESENT_PARAMETERS* pp) {
    D3DPRESENT_PARAMETERS mine = *pp;
    FixPresentParameters(&mine);
    HRESULT hr = Real_Reset(self, &mine);
    Log(L"D3D: Reset %ux%u %s -> %#lx", mine.BackBufferWidth, mine.BackBufferHeight,
        mine.Windowed ? L"windowed" : L"fullscreen", hr);
    return hr;
}

LARGE_INTEGER g_freq, g_lastFrame, g_statStart;
int g_statFrames = 0;
using DwmFlush_t = HRESULT(WINAPI*)();
DwmFlush_t g_dwmFlush = nullptr;

void PaceFrame() {
    int fps = kFrameRates[g_frameRate].fps;
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

HRESULT STDMETHODCALLTYPE Hook_Present(IDirect3DDevice9* self, const RECT* src, const RECT* dst, HWND wnd,
                                       const RGNDATA* dirty) {
    HRESULT hr = Real_Present(self, src, dst, wnd, dirty);
    PaceFrame();
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

HRESULT STDMETHODCALLTYPE Hook_CreateDevice(IDirect3D9* self, UINT adapter, D3DDEVTYPE type, HWND focus,
                                            DWORD flags, D3DPRESENT_PARAMETERS* pp, IDirect3DDevice9** out) {
    D3DPRESENT_PARAMETERS original = *pp;  // CreateDevice may modify it; retry with what the game asked
    HRESULT hr = E_FAIL;
    for (int attempt = 1; attempt <= 20; ++attempt) {
        *pp = original;
        FixPresentParameters(pp);
        hr = Real_CreateDevice(self, adapter, type, focus, flags, pp, out);
        Log(L"D3D: CreateDevice %ux%u %s -> %#lx%s", original.BackBufferWidth, original.BackBufferHeight,
            original.Windowed ? L"windowed" : L"fullscreen", hr, attempt > 1 ? L" (retry)" : L"");
        if (SUCCEEDED(hr)) break;
        Sleep(250);
    }
    if (SUCCEEDED(hr) && out && *out) {
        if (void* o = PatchVtable(*out, 16, (void*)Hook_Reset)) Real_Reset = (Reset_t)o;
        if (void* o = PatchVtable(*out, 17, (void*)Hook_Present)) Real_Present = (Present_t)o;
    }
    return hr;
}

IDirect3D9* WINAPI Hook_Direct3DCreate9(UINT sdk) {
    IDirect3D9* d3d = Real_Direct3DCreate9(sdk);
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
    if (HMODULE dwm = LoadLibraryW(L"dwmapi.dll")) g_dwmFlush = (DwmFlush_t)GetProcAddress(dwm, "DwmFlush");

    HookSpec hooks[] = {
        {"d3d9.dll", "Direct3DCreate9", (void*)Hook_Direct3DCreate9, (void**)&Real_Direct3DCreate9},
        {"user32.dll", "EnumDisplaySettingsW", (void*)Hook_EnumDisplaySettingsW, (void**)&Real_EnumDisplaySettingsW},
    };
    InstallHooks(exe, hooks);
}
