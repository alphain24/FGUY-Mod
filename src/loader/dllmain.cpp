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
#include <intrin.h>

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

// Research (FGUYMOD_TRACE_FREE=1): who frees what. The game's free (0x428c70, cdecl: the block, a flag) reads the block's header - a block freed twice, its
// memory already given back, crashes there (0x428c9b) with nothing on the stack to say who freed it first: big blocks are freed on a thread of their own.
// The last two million frees are kept - the block, who called, the thread, when - and a crash there lists the ones of that block.
struct Freed { void* p; void* from; DWORD thread, tick; };
Freed g_freed[1 << 21];
volatile LONG g_freedAt = 0;
bool g_freeTrace = false;
using GameFree_t = void(__cdecl*)(void*, int);
GameFree_t g_gameFree = nullptr;
void __cdecl Hook_GameFree(void* block, int flag) {
    if (block) {
        LONG i = InterlockedIncrement(&g_freedAt) & ((1 << 21) - 1);
        g_freed[i].p = block, g_freed[i].from = _ReturnAddress(), g_freed[i].thread = GetCurrentThreadId(), g_freed[i].tick = GetTickCount();
    }
    g_gameFree(block, flag);
}
// (...and the threads the game starts through its C runtime - what each runs, what it's given, who started it: the block's freed on a thread that does
// nothing else by then. The game's imports of _beginthreadex and _beginthread, put through here)
struct Started { void* start; void* arg; void* from; DWORD tick; int ex; unsigned id; };
Started g_started[256];
volatile LONG g_startedAt = 0;
using BeginEx_t = uintptr_t(__cdecl*)(void*, unsigned, unsigned(__stdcall*)(void*), void*, unsigned, unsigned*);
using Begin_t = uintptr_t(__cdecl*)(void(__cdecl*)(void*), unsigned, void*);
BeginEx_t g_beginEx = nullptr;
Begin_t g_begin = nullptr;
void NoteStarted(void* start, void* arg, void* from, int ex, unsigned id) {
    LONG i = InterlockedIncrement(&g_startedAt) & 255;
    g_started[i].start = start, g_started[i].arg = arg, g_started[i].from = from, g_started[i].tick = GetTickCount(), g_started[i].ex = ex, g_started[i].id = id;
}
uintptr_t __cdecl Hook_BeginEx(void* security, unsigned stack, unsigned(__stdcall* start)(void*), void* arg, unsigned flag, unsigned* id) {
    unsigned mine = 0;
    uintptr_t r = g_beginEx(security, stack, start, arg, flag, id ? id : &mine);
    NoteStarted(reinterpret_cast<void*>(start), arg, _ReturnAddress(), 1, id ? *id : mine);
    return r;
}
uintptr_t __cdecl Hook_Begin(void(__cdecl* start)(void*), unsigned stack, void* arg) {
    NoteStarted(reinterpret_cast<void*>(start), arg, _ReturnAddress(), 0, 0);
    return g_begin(start, stack, arg);
}
void** GameImport(const char* dll, const char* name) {
    auto base = reinterpret_cast<unsigned char*>(GetModuleHandleW(nullptr));
    auto nt = reinterpret_cast<IMAGE_NT_HEADERS*>(base + reinterpret_cast<IMAGE_DOS_HEADER*>(base)->e_lfanew);
    const IMAGE_DATA_DIRECTORY& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!dir.VirtualAddress) return nullptr;
    for (auto d = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(base + dir.VirtualAddress); d->Name; ++d) {
        if (_stricmp(reinterpret_cast<const char*>(base + d->Name), dll) != 0 || !d->OriginalFirstThunk) continue;
        auto names = reinterpret_cast<IMAGE_THUNK_DATA*>(base + d->OriginalFirstThunk);
        auto slots = reinterpret_cast<IMAGE_THUNK_DATA*>(base + d->FirstThunk);
        for (; names->u1.AddressOfData; ++names, ++slots) {
            if (names->u1.Ordinal & IMAGE_ORDINAL_FLAG) continue;
            auto by = reinterpret_cast<IMAGE_IMPORT_BY_NAME*>(base + names->u1.AddressOfData);
            if (strcmp(by->Name, name) == 0) return reinterpret_cast<void**>(&slots->u1.Function);
        }
    }
    return nullptr;
}
// (...and the runtime's own free, the game's import at 0x9dc318 - what the game's free ends in, and what other code of the game's may call by itself:
// the block that crashes was freed only once through the game's free, yet its pages were gone)
struct RawFreed { void* p; void* from; DWORD thread, tick; };
RawFreed g_rawFreed[1 << 20];
volatile LONG g_rawFreedAt = 0;
using CrtFree_t = void(__cdecl*)(void*);
CrtFree_t g_crtFree = nullptr;
void __cdecl Hook_CrtFree(void* p) {
    if (p) {
        LONG i = InterlockedIncrement(&g_rawFreedAt) & ((1 << 20) - 1);
        g_rawFreed[i].p = p, g_rawFreed[i].from = _ReturnAddress(), g_rawFreed[i].thread = GetCurrentThreadId(), g_rawFreed[i].tick = GetTickCount();
    }
    g_crtFree(p);
}
void InitFreeTrace() {
    wchar_t v[4] = L"";
    if (!GetEnvironmentVariableW(L"FGUYMOD_TRACE_FREE", v, 4) || v[0] != L'1') return;
    // (the runtime's own _beginthreadex, whoever calls it: the game's import alone showed two threads, neither the one that crashes - another module in
    // the game shares the runtime. Its first three instructions - mov edi, edi; push ebp; mov ebp, esp - moved to a stub)
    wchar_t threads[4] = L"";
    GetEnvironmentVariableW(L"FGUYMOD_TRACE_THREADS", threads, 4);
    if (HMODULE crt = threads[0] == L'1' ? GetModuleHandleW(L"MSVCR100.dll") : nullptr) {
        auto fn = reinterpret_cast<unsigned char*>(GetProcAddress(crt, "_beginthreadex"));
        if (fn && fn[0] == 0x8b && fn[1] == 0xff && fn[2] == 0x55 && fn[3] == 0x8b && fn[4] == 0xec) {
            auto stub = static_cast<unsigned char*>(VirtualAlloc(nullptr, 32, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
            if (stub) {
                memcpy(stub, fn, 5);
                stub[5] = 0xe9;
                *reinterpret_cast<int*>(stub + 6) = static_cast<int>((fn + 5) - (stub + 10));
                g_beginEx = reinterpret_cast<BeginEx_t>(stub);
                DWORD was = 0;
                VirtualProtect(fn, 5, PAGE_EXECUTE_READWRITE, &was);
                fn[0] = 0xe9;
                *reinterpret_cast<int*>(fn + 1) = static_cast<int>(reinterpret_cast<unsigned char*>(&Hook_BeginEx) - (fn + 5));
                
                VirtualProtect(fn, 5, was, &was);
                Log(L"Research: the runtime's _beginthreadex is traced");
            }
        } else Log(L"Research: _beginthreadex doesn't start as expected (%02x %02x %02x) - not traced", fn ? fn[0] : 0, fn ? fn[1] : 0, fn ? fn[2] : 0);
    }
    (void)GameImport;
    (void)Hook_Begin;
    {
        auto slot = reinterpret_cast<void**>(0x9dc318);
        DWORD was = 0;
        if (!IsBadReadPtr(slot, 4) && VirtualProtect(slot, 4, PAGE_READWRITE, &was)) {
            g_crtFree = reinterpret_cast<CrtFree_t>(*slot);
            *slot = reinterpret_cast<void*>(&Hook_CrtFree);
            VirtualProtect(slot, 4, was, &was);
            Log(L"Research: the runtime's free (the game's import) is traced");
        }
    }
    auto at = reinterpret_cast<unsigned char*>(0x428c70);
    static const unsigned char start[7] = {0x55, 0x8b, 0xec, 0x56, 0x8b, 0x75, 0x08};   // push ebp; mov ebp, esp; push esi; mov esi, [ebp+8]
    if (IsBadReadPtr(at, 7) || memcmp(at, start, 7) != 0) { Log(L"Research: the game's free isn't where it's expected - not traced"); return; }
    auto t = static_cast<unsigned char*>(VirtualAlloc(nullptr, 32, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
    if (!t) return;
    memcpy(t, at, 7);
    t[7] = 0xe9;
    *reinterpret_cast<int*>(t + 8) = static_cast<int>((at + 7) - (t + 12));
    g_gameFree = reinterpret_cast<GameFree_t>(t);
    DWORD old = 0;
    VirtualProtect(at, 7, PAGE_EXECUTE_READWRITE, &old);
    at[0] = 0xe9;
    *reinterpret_cast<int*>(at + 1) = static_cast<int>(reinterpret_cast<unsigned char*>(&Hook_GameFree) - (at + 5));
    at[5] = at[6] = 0x90;
    VirtualProtect(at, 7, old, &old);
    g_freeTrace = true;
    Log(L"Research: the game's frees are traced");
}

// A crash in the game's code: where, and the game addresses on the stack (its callers) - into the log, for
// finding out why. Only writes the log (the first few times); the crash goes on as it would.
LONG CALLBACK CrashLogHandler(EXCEPTION_POINTERS* e) {
    static volatile LONG count = 0;
    if (!e || !e->ExceptionRecord || e->ExceptionRecord->ExceptionCode != EXCEPTION_ACCESS_VIOLATION) return EXCEPTION_CONTINUE_SEARCH;
    const DWORD kCodeStart = 0x401000, kCodeEnd = 0x9f0000;
    const CONTEXT* c = e->ContextRecord;
    // (A fault outside the game's own code - in the loader, a plugin, Direct3D - is said too, with the module it's in: the game turns a fault in its loop
    // into a C++ exception (_set_se_translator, 0x97d790), catches it in main and exits as if asked to; what was logged then was only the crash its
    // exit-time cleanup always has (0x428c9b), not the fault that made it leave. IsBadReadPtr's own probing faults are passed over: handled where they are.)
    if (c->Eip < kCodeStart || c->Eip >= kCodeEnd) {
        static volatile LONG others = 0;
        HMODULE m = nullptr;
        wchar_t path[MAX_PATH] = L"?";
        if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, reinterpret_cast<LPCWSTR>(static_cast<uintptr_t>(c->Eip)), &m) && m) GetModuleFileNameW(m, path, MAX_PATH);
        const wchar_t* name = wcsrchr(path, L'\\');
        name = name ? name + 1 : path;
        if (_wcsicmp(name, L"KERNELBASE.dll") == 0 || _wcsicmp(name, L"KERNEL32.DLL") == 0) return EXCEPTION_CONTINUE_SEARCH;   // (IsBadReadPtr)
        if (InterlockedIncrement(&others) > 6) return EXCEPTION_CONTINUE_SEARCH;
        Log(L"FAULT in %s+%#lx (%s %#lx) eax=%#lx ecx=%#lx edx=%#lx esi=%#lx edi=%#lx thread %lu", name, m ? c->Eip - (DWORD)reinterpret_cast<uintptr_t>(m) : c->Eip,
            e->ExceptionRecord->ExceptionInformation[0] ? L"writing" : L"reading", (DWORD)e->ExceptionRecord->ExceptionInformation[1], c->Eax, c->Ecx, c->Edx, c->Esi, c->Edi, GetCurrentThreadId());
        const DWORD* sp2 = reinterpret_cast<const DWORD*>(c->Esp);
        const DWORD* top2 = reinterpret_cast<const DWORD*>(reinterpret_cast<NT_TIB*>(NtCurrentTeb())->StackBase);
        wchar_t line2[1024];
        int len2 = 0, found2 = 0;
        line2[0] = 0;
        for (; sp2 < top2 && sp2 < reinterpret_cast<const DWORD*>(c->Esp) + 2048 && found2 < 24 && len2 < 900; ++sp2) {
            if (*sp2 >= kCodeStart && *sp2 < kCodeEnd) { len2 += swprintf_s(line2 + len2, 1024 - len2, L" %#lx", *sp2); ++found2; continue; }
            HMODULE m2 = nullptr;
            if (*sp2 < 0x10000 || !GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, reinterpret_cast<LPCWSTR>(static_cast<uintptr_t>(*sp2)), &m2) || m2 != m) continue;
            len2 += swprintf_s(line2 + len2, 1024 - len2, L" %s+%#lx", name, *sp2 - (DWORD)reinterpret_cast<uintptr_t>(m2));
            ++found2;
        }
        Log(L"FAULT stack (the game's addresses, and that module's):%s", line2);
        return EXCEPTION_CONTINUE_SEARCH;
    }
    if (InterlockedIncrement(&count) > 3) return EXCEPTION_CONTINUE_SEARCH;
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
    // (and what else on the stack points into a loaded module's code - the loader's own, a plugin's, Direct3D's: who called into the game. The game's own
    // addresses alone showed a free with nothing between it and main)
    sp = reinterpret_cast<const DWORD*>(c->Esp);
    len = 0, found = 0;
    line[0] = 0;
    for (; sp < top && sp < reinterpret_cast<const DWORD*>(c->Esp) + 4096 && found < 28; ++sp) {
        if (*sp < 0x10000 || (*sp >= kCodeStart && *sp < kCodeEnd)) continue;
        HMODULE m = nullptr;
        if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, reinterpret_cast<LPCWSTR>(static_cast<uintptr_t>(*sp)), &m) || !m) continue;
        MEMORY_BASIC_INFORMATION mi{};
        if (!VirtualQuery(reinterpret_cast<void*>(static_cast<uintptr_t>(*sp)), &mi, sizeof(mi)) || !(mi.Protect & (PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE))) continue;
        wchar_t path[MAX_PATH] = L"";
        GetModuleFileNameW(m, path, MAX_PATH);
        const wchar_t* name = wcsrchr(path, L'\\');
        len += swprintf_s(line + len, 1024 - len, L" %s+%#lx@%d", name ? name + 1 : path, *sp - (DWORD)reinterpret_cast<uintptr_t>(m), (int)(sp - reinterpret_cast<const DWORD*>(c->Esp)));
        ++found;
        if (len > 900) break;
    }
    Log(L"CRASH stack (other modules, +offset@stack slot):%s", line);
    if (g_freeTrace) {   // (the block being freed is in esi there, or the address read + 4)
        void* block = reinterpret_cast<void*>(static_cast<uintptr_t>(e->ExceptionRecord->ExceptionInformation[1] + 4));
        DWORD now = GetTickCount();
        int said = 0;
        for (int i = 0; i < (1 << 21) && said < 6; ++i) {
            const Freed& f = g_freed[(g_freedAt - i) & ((1 << 21) - 1)];
            if (f.p != block) continue;
            HMODULE m = nullptr;
            wchar_t path[MAX_PATH] = L"?";
            if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, static_cast<LPCWSTR>(f.from), &m) && m) GetModuleFileNameW(m, path, MAX_PATH);
            const wchar_t* name = wcsrchr(path, L'\\');
            Log(L"CRASH free of %p - %d frees ago, %lu ms ago, thread %lu, called from %p (%s+%#lx)", block, i, now - f.tick, f.thread, f.from, name ? name + 1 : path,
                m ? (DWORD)(reinterpret_cast<uintptr_t>(f.from) - reinterpret_cast<uintptr_t>(m)) : 0);
            ++said;
        }
        if (!said) Log(L"CRASH free of %p - not among the last two million frees", block);
        // (the runtime's frees of anything in the 64 KB before the block: its real start, or what it lay inside)
        said = 0;
        for (int i = 0; i < (1 << 20) && said < 8; ++i) {
            const RawFreed& f = g_rawFreed[(g_rawFreedAt - i) & ((1 << 20) - 1)];
            auto d = reinterpret_cast<uintptr_t>(block) - reinterpret_cast<uintptr_t>(f.p);
            if (!f.p || d > 0x10000) continue;
            Log(L"CRASH runtime free of %p (%#lx before the block) - %d frees ago, %lu ms ago, thread %lu, called from %p", f.p, (DWORD)d, i, now - f.tick, f.thread, f.from);
            ++said;
        }
        if (!said) Log(L"CRASH no runtime free of anything in the 64 KB before the block among the last million");
        for (int i = 0; i < 256; ++i) {
            const Started& t = g_started[(g_startedAt - i) & 255];
            if (i >= 8 && t.id != GetCurrentThreadId()) continue;
            if (!t.start) continue;
            auto in = [](void* a, wchar_t* out) {
                HMODULE m = nullptr;
                wchar_t path[MAX_PATH] = L"?";
                if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, static_cast<LPCWSTR>(a), &m) && m) GetModuleFileNameW(m, path, MAX_PATH);
                const wchar_t* name = wcsrchr(path, L'\\');
                swprintf_s(out, 96, L"%s+%#lx", name ? name + 1 : path, m ? (DWORD)(reinterpret_cast<uintptr_t>(a) - reinterpret_cast<uintptr_t>(m)) : 0);
            };
            wchar_t ws[96], wf[96];
            in(t.start, ws), in(t.from, wf);
            Log(L"CRASH thread %u%s started %lu ms ago: runs %s, arg %p%s, started from %s", t.id, t.id == GetCurrentThreadId() ? L" (THIS ONE)" : L"", now - t.tick, ws, t.arg, t.arg == block ? L" - THE BLOCK" : L"", wf);
        }
    }
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
    InitFreeTrace();

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
    InitSteadyClock(exe);
    InitDirectInput();
    InitNetSpy(exe);
    InitVirtualPad(exe);
    InitUi();
    InitResearch();
    InitMotion();
    InitFirstPerson();
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
