// FGUY Mod Loader
//
// Built as dinput8.dll and dropped next to FGUY.exe. The game imports DirectInput8Create from
// dinput8.dll, so Windows loads this DLL first; we forward that call to the real system DLL and
// use the chance to install our hooks before the game's own code runs.
//
// Modules:
//   config.cpp   game config override (FGUYMod\fguy.ini) + file-open log
//   display.cpp  fullscreen / windowed / borderless, render resolution
//   netspy.cpp   logs the game's network traffic and debug prints (research)
//   d3d9.cpp     retries Direct3D device creation (the game crashes if it fails)
//   dinput.cpp   lets the Windows key work in windowed / borderless modes
//   ui.cpp       menu movie patches (e.g. Borderless in Options) + menu-to-loader calls
//   netplay.cpp  online play core: lockstep input sync, fixed clock, desync checks
//   vpad.cpp     virtual controller for test scripts (and later, remote players)
// Loader settings: FGUYMod\FGUYMod.ini. Log: FGUYMod\FGUYMod_<pid>.log.

#include "common.h"

#include <cwchar>
#include <unknwn.h>

namespace {

HMODULE g_realDinput = nullptr;
using DirectInput8Create_t = HRESULT(WINAPI*)(HINSTANCE, DWORD, REFIID, LPVOID*, LPUNKNOWN);
DirectInput8Create_t Real_DirectInput8Create = nullptr;

bool LoadRealDinput() {
    wchar_t path[MAX_PATH];
    GetSystemDirectoryW(path, MAX_PATH);  // SysWOW64 for a 32-bit process
    wcscat_s(path, L"\\dinput8.dll");
    g_realDinput = LoadLibraryW(path);
    if (!g_realDinput) return false;
    Real_DirectInput8Create =
        reinterpret_cast<DirectInput8Create_t>(GetProcAddress(g_realDinput, "DirectInput8Create"));
    return Real_DirectInput8Create != nullptr;
}

void Init() {
    GetModuleFileNameW(nullptr, g_gameDir, MAX_PATH);
    if (wchar_t* slash = wcsrchr(g_gameDir, L'\\')) *slash = 0;
    swprintf_s(g_modDir, L"%s\\FGUYMod", g_gameDir);
    CreateDirectoryW(g_modDir, nullptr);

    OpenLog();
    Log(L"FGUY Mod Loader starting (main thread %lu). Game folder: %s", GetCurrentThreadId(), g_gameDir);
    if (!LoadRealDinput()) Log(L"ERROR: could not load the system dinput8.dll");

    HMODULE exe = GetModuleHandleW(nullptr);
    InitNetplay(exe);
    InitConfigOverride(exe);
    InitDisplay(exe);
    InitD3D9(exe);
    InitDirectInput();
    InitNetSpy(exe);
    InitVirtualPad(exe);
    InitUi();
    InitResearch();
    InitSplitView();
}

}  // namespace

extern "C" HRESULT WINAPI Proxy_DirectInput8Create(HINSTANCE inst, DWORD version, REFIID riid,
                                                   LPVOID* out, LPUNKNOWN outer) {
    if (!Real_DirectInput8Create) return E_FAIL;
    HRESULT hr = Real_DirectInput8Create(inst, version, riid, out, outer);
    if (SUCCEEDED(hr) && out) OnDirectInputCreated(*out);
    return hr;
}

BOOL WINAPI DllMain(HMODULE self, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(self);
        Init();
    }
    return TRUE;
}
