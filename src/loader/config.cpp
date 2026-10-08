// Game config override + file-open logging.
//
// The retail exe reads its settings from a built-in resource (type "INI", id 101) and only falls
// back to an external ./fguy.ini when that resource is missing (that's how the developers' builds
// worked). If FGUYMod\fguy.ini exists, we hide the resource and point the game's ./fguy.ini lookup
// at our file. The FGUYMOD_INI environment variable picks a different file inside FGUYMod (used to
// run a server and a client side by side; not a command-line switch, because the game quits on
// arguments it doesn't recognize).

#include "common.h"

#include <cwchar>

namespace {

wchar_t g_iniPath[MAX_PATH];
bool g_iniOverride = false;
int g_fileLogBudget = 2000;  // keep the log readable

bool EndsWithFguyIni(const wchar_t* path) {
    size_t n = wcslen(path);
    const size_t k = wcslen(L"fguy.ini");
    if (n < k || _wcsicmp(path + n - k, L"fguy.ini") != 0) return false;
    return n == k || path[n - k - 1] == L'\\' || path[n - k - 1] == L'/';
}

bool EndsWithFguyIniA(const char* path) {
    wchar_t w[MAX_PATH * 2];
    MultiByteToWideChar(CP_ACP, 0, path, -1, w, MAX_PATH * 2);
    return EndsWithFguyIni(w);
}

decltype(&FindResourceW) Real_FindResourceW;
decltype(&CreateFileW) Real_CreateFileW;
decltype(&CreateFileA) Real_CreateFileA;

HRSRC WINAPI Hook_FindResourceW(HMODULE module, LPCWSTR name, LPCWSTR type) {
    bool isIni = !IS_INTRESOURCE(type) && _wcsicmp(type, L"INI") == 0 &&
                 IS_INTRESOURCE(name) && reinterpret_cast<ULONG_PTR>(name) == 101;
    if (isIni && g_iniOverride) {
        Log(L"Config: hiding built-in INI resource so the game reads the external fguy.ini");
        SetLastError(ERROR_RESOURCE_TYPE_NOT_FOUND);
        return nullptr;
    }
    return Real_FindResourceW(module, name, type);
}

HANDLE WINAPI Hook_CreateFileW(LPCWSTR path, DWORD access, DWORD share, LPSECURITY_ATTRIBUTES sa,
                               DWORD disposition, DWORD flags, HANDLE templ) {
    LPCWSTR actual = path;
    if (g_iniOverride && path && EndsWithFguyIni(path)) {
        Log(L"Config: game asked for \"%s\" -> redirected to \"%s\"", path, g_iniPath);
        actual = g_iniPath;
    }
    HANDLE h = Real_CreateFileW(actual, access, share, sa, disposition, flags, templ);
    if (path && g_fileLogBudget > 0) {
        --g_fileLogBudget;
        Log(L"Open%s \"%s\"%s", (access & GENERIC_WRITE) ? L"(write)" : L"", path,
            h == INVALID_HANDLE_VALUE ? L"  [not found]" : L"");
    }
    return h;
}

HANDLE WINAPI Hook_CreateFileA(LPCSTR path, DWORD access, DWORD share, LPSECURITY_ATTRIBUTES sa,
                               DWORD disposition, DWORD flags, HANDLE templ) {
    if (g_iniOverride && path && EndsWithFguyIniA(path)) {
        Log(L"Config: game asked for \"%S\" -> redirected to \"%s\"", path, g_iniPath);
        return Hook_CreateFileW(g_iniPath, access, share, sa, disposition, flags, templ);
    }
    HANDLE h = Real_CreateFileA(path, access, share, sa, disposition, flags, templ);
    if (path && g_fileLogBudget > 0) {
        --g_fileLogBudget;
        Log(L"OpenA \"%S\"%s", path, h == INVALID_HANDLE_VALUE ? L"  [not found]" : L"");
    }
    return h;
}

}  // namespace

void InitConfigOverride(HMODULE exe) {
    wchar_t iniName[MAX_PATH] = L"fguy.ini";
    wchar_t env[MAX_PATH];
    DWORD envLen = GetEnvironmentVariableW(L"FGUYMOD_INI", env, MAX_PATH);
    if (envLen > 0 && envLen < MAX_PATH) wcscpy_s(iniName, env);
    swprintf_s(g_iniPath, L"%s\\%s", g_modDir, iniName);
    g_iniOverride = FileExists(g_iniPath);
    Log(g_iniOverride ? L"Config override ON: %s" : L"Config override off (no %s)", g_iniPath);

    HookSpec hooks[] = {
        {"kernel32.dll", "FindResourceW", (void*)Hook_FindResourceW, (void**)&Real_FindResourceW},
        {"kernel32.dll", "CreateFileW", (void*)Hook_CreateFileW, (void**)&Real_CreateFileW},
        {"kernel32.dll", "CreateFileA", (void*)Hook_CreateFileA, (void**)&Real_CreateFileA},
    };
    InstallHooks(exe, hooks);
}
