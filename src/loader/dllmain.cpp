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
//   online.cpp   the lobby; handoff.cpp starts its sessions without the game visibly restarting
//   sounds.cpp   a Family Guy sting on loading screens
//   kbm.cpp      keyboard and mouse as this player's controller in online sessions
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

// A crash in the game's code: where, and the game addresses on the stack (its callers) - into the log, for
// finding out why. Only writes the log (the first few times); the crash goes on as it would.
LONG CALLBACK CrashLogHandler(EXCEPTION_POINTERS* e) {
    static volatile LONG count = 0;
    if (!e || !e->ExceptionRecord || e->ExceptionRecord->ExceptionCode != EXCEPTION_ACCESS_VIOLATION) return EXCEPTION_CONTINUE_SEARCH;
    const DWORD kCodeStart = 0x401000, kCodeEnd = 0x9f0000;
    const CONTEXT* c = e->ContextRecord;
    if (c->Eip < kCodeStart || c->Eip >= kCodeEnd || InterlockedIncrement(&count) > 3) return EXCEPTION_CONTINUE_SEARCH;
    Log(L"CRASH at %#lx (%s %#lx) eax=%#lx ebx=%#lx ecx=%#lx edx=%#lx esi=%#lx edi=%#lx ebp=%#lx esp=%#lx thread %lu",
        c->Eip, e->ExceptionRecord->ExceptionInformation[0] ? L"writing" : L"reading",
        (DWORD)e->ExceptionRecord->ExceptionInformation[1], c->Eax, c->Ebx, c->Ecx, c->Edx, c->Esi, c->Edi, c->Ebp,
        c->Esp, GetCurrentThreadId());
    const DWORD* sp = reinterpret_cast<const DWORD*>(c->Esp);
    const DWORD* top = reinterpret_cast<const DWORD*>(reinterpret_cast<NT_TIB*>(NtCurrentTeb())->StackBase);
    wchar_t line[1024];
    int len = 0, found = 0;
    for (; sp < top && sp < reinterpret_cast<const DWORD*>(c->Esp) + 1024 && found < 40; ++sp)
        if (*sp >= kCodeStart && *sp < kCodeEnd) {
            len += swprintf_s(line + len, 1024 - len, L" %#lx", *sp);
            ++found;
        }
    Log(L"CRASH stack (game addresses):%s", line);
    return EXCEPTION_CONTINUE_SEARCH;
}

void Init() {
    GetModuleFileNameW(nullptr, g_gameDir, MAX_PATH);
    if (wchar_t* slash = wcsrchr(g_gameDir, L'\\')) *slash = 0;
    swprintf_s(g_modDir, L"%s\\FGUYMod", g_gameDir);
    CreateDirectoryW(g_modDir, nullptr);

    OpenLog();
    Log(L"FGUY Mod Loader starting (main thread %lu). Game folder: %s", GetCurrentThreadId(), g_gameDir);
    if (!LoadRealDinput()) Log(L"ERROR: could not load the system dinput8.dll");
    AddVectoredExceptionHandler(0, CrashLogHandler);

    HMODULE exe = GetModuleHandleW(nullptr);
    InitNetplay(exe);
    InitConfigOverride(exe);
    InitHandoff();
    InitReports();
    InitMods();
    InitUpdater();
    InitAudio(exe);
    InitSounds();
    InitDisplay(exe);
    InitD3D9(exe);
    InitDirectInput();
    InitNetSpy(exe);
    InitVirtualPad(exe);
    InitUi();
    InitResearch();
    InitMotion();
    InitModels();
    InitSplitView();
    InitRngTrace();
    InitLoadSync();
    InitOnlineMenu();
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
