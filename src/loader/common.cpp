#include "common.h"

#include <cstdarg>
#include <cstdio>
#include <share.h>
#include <cstdlib>

#include "iat_hook.h"

wchar_t g_gameDir[MAX_PATH];
wchar_t g_modDir[MAX_PATH];

namespace {
FILE* g_log = nullptr;
CRITICAL_SECTION g_logLock;
}  // namespace

void OpenLog() {
    InitializeCriticalSection(&g_logLock);
    wchar_t path[MAX_PATH];
    // One log per running copy, so two copies on the same PC don't clash.
    swprintf_s(path, L"%s\\FGUYMod_%lu.log", g_modDir, GetCurrentProcessId());
    g_log = _wfsopen(path, L"w, ccs=UTF-8", _SH_DENYNO);  // readable while the game runs
}

void Log(const wchar_t* fmt, ...) {
    if (!g_log) return;
    EnterCriticalSection(&g_logLock);
    SYSTEMTIME t;
    GetLocalTime(&t);
    fwprintf(g_log, L"[%02d:%02d:%02d.%03d] ", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds);
    va_list args;
    va_start(args, fmt);
    vfwprintf(g_log, fmt, args);
    va_end(args);
    fputwc(L'\n', g_log);
    fflush(g_log);
    LeaveCriticalSection(&g_logLock);
}

bool FileExists(const wchar_t* path) {
    DWORD a = GetFileAttributesW(path);
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

static void SettingsPath(wchar_t* out) { swprintf_s(out, MAX_PATH, L"%s\\FGUYMod.ini", g_modDir); }

int SettingInt(const wchar_t* section, const wchar_t* key, const wchar_t* envVar, int def) {
    wchar_t v[64];
    if (envVar && GetEnvironmentVariableW(envVar, v, 64)) return _wtoi(v);
    wchar_t path[MAX_PATH];
    SettingsPath(path);
    return (int)GetPrivateProfileIntW(section, key, def, path);
}

void SettingString(const wchar_t* section, const wchar_t* key, const wchar_t* envVar,
                   const wchar_t* def, wchar_t* out, DWORD outLen) {
    if (envVar) {
        DWORD n = GetEnvironmentVariableW(envVar, out, outLen);
        if (n > 0 && n < outLen) return;
    }
    wchar_t path[MAX_PATH];
    SettingsPath(path);
    GetPrivateProfileStringW(section, key, def, out, outLen, path);
    // Allow trailing "; comments" on the same line.
    if (wchar_t* semi = wcschr(out, L';')) *semi = 0;
    for (size_t n = wcslen(out); n && (out[n - 1] == L' ' || out[n - 1] == L'\t'); --n) out[n - 1] = 0;
}

void InstallHooks(HMODULE exe, HookSpec* hooks, size_t count) {
    for (size_t i = 0; i < count; ++i) {
        HookSpec& h = hooks[i];
        bool ok = HookIat(exe, h.dll, h.fn, h.hook, h.real);
        if (!ok) {
            // Not imported by the exe: still make the "real" pointer usable for our own calls.
            *h.real = (void*)GetProcAddress(GetModuleHandleA(h.dll), h.fn);
        }
        if (IS_INTRESOURCE(h.fn))
            Log(L"Hook %S!#%u: %s", h.dll, (unsigned)reinterpret_cast<ULONG_PTR>(h.fn),
                ok ? L"installed" : L"not imported by exe");
        else
            Log(L"Hook %S!%S: %s", h.dll, h.fn, ok ? L"installed" : L"not imported by exe");
    }
}
