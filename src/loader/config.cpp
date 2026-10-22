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
#include <string>

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

// Test runs (env FGUYMOD_SAVEDIR=<folder name inside FGUYMod>) keep their saves away from the real
// ones: ".\Save\<file>" goes to FGUYMod\<folder>\<file>, starting from a copy of the real file.
wchar_t g_saveDir[MAX_PATH] = L"";

bool RedirectSave(LPCWSTR path, wchar_t* out) {
    if (!g_saveDir[0] || !path) return false;
    const wchar_t* rel = path;
    if (rel[0] == L'.' && (rel[1] == L'\\' || rel[1] == L'/')) rel += 2;
    if (_wcsnicmp(rel, L"Save\\", 5) != 0 && _wcsnicmp(rel, L"Save/", 5) != 0) return false;
    swprintf_s(out, MAX_PATH, L"%s\\%s", g_saveDir, rel + 5);
    if (!FileExists(out)) {
        wchar_t real[MAX_PATH];
        swprintf_s(real, L"%s\\Save\\%s", g_gameDir, rel + 5);
        CopyFileW(real, out, TRUE);
    }
    return true;
}

HANDLE WINAPI Hook_CreateFileW(LPCWSTR path, DWORD access, DWORD share, LPSECURITY_ATTRIBUTES sa,
                               DWORD disposition, DWORD flags, HANDLE templ) {
    LPCWSTR actual = path;
    wchar_t saveRedirect[MAX_PATH];
    if (g_iniOverride && path && EndsWithFguyIni(path)) {
        Log(L"Config: game asked for \"%s\" -> redirected to \"%s\"", path, g_iniPath);
        actual = g_iniPath;
    } else if (RedirectSave(path, saveRedirect)) {
        actual = saveRedirect;
    } else if (!(access & GENERIC_WRITE) && ModsFileOverride(path, saveRedirect)) {   // (a mod's copy of the file)
        actual = saveRedirect;
    }
    if (!(access & GENERIC_WRITE) && actual && wcsstr(actual, L"player0_default.sav")) NetplayFetchSave(actual);
    HANDLE h = Real_CreateFileW(actual, access, share, sa, disposition, flags, templ);
    if (h != INVALID_HANDLE_VALUE) SoundsOnFileOpen(path);  // a level's package: loading music
    if (h != INVALID_HANDLE_VALUE) NetplayLevelFileOpened(path);   // (story co-op: a level the game loads by itself)
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
    wchar_t wide[MAX_PATH], mod[MAX_PATH];
    if (path && !(access & GENERIC_WRITE) && MultiByteToWideChar(CP_ACP, 0, path, -1, wide, MAX_PATH) &&
        ModsFileOverride(wide, mod))   // (a mod's copy of the file)
        return Real_CreateFileW(mod, access, share, sa, disposition, flags, templ);
    HANDLE h = Real_CreateFileA(path, access, share, sa, disposition, flags, templ);
    if (path && g_fileLogBudget > 0) {
        --g_fileLogBudget;
        Log(L"OpenA \"%S\"%s", path, h == INVALID_HANDLE_VALUE ? L"  [not found]" : L"");
    }
    return h;
}

}  // namespace

const wchar_t* ConfigSaveDir() {
    static wchar_t real[MAX_PATH];
    if (g_saveDir[0]) return g_saveDir;
    swprintf_s(real, L"%s\\Save", g_gameDir);
    return real;
}

// Online sessions boot without the logo / intro movies (everyone's game goes straight to the menus,
// where the session takes over) and without the developers' network link. We make that config from
// the game's own built-in one at runtime: FGUYMod\<session folder>\session_fguy.ini.
bool MakeSessionIni(HMODULE exe, const wchar_t* folder, wchar_t* out) {
    HRSRC r = FindResourceW(exe, MAKEINTRESOURCEW(101), L"INI");
    HGLOBAL g = r ? LoadResource(exe, r) : nullptr;
    const char* text = g ? static_cast<const char*>(LockResource(g)) : nullptr;
    DWORD size = r ? SizeofResource(exe, r) : 0;
    if (!text || !size) return false;
    std::string ini(text, size);
    auto set = [&](const char* key, const char* value) {
        size_t at = ini.find(std::string("\n") + key);
        if (at == std::string::npos) return;
        size_t eq = ini.find('=', at), end = ini.find_first_of("\r\n", at + 1);
        if (eq == std::string::npos || eq > end) return;
        ini.replace(eq + 1, end - eq - 1, std::string(" ") + value);
    };
    set("NoMovies", "1");
    set("IsServer", "0");
    set("EnableNetworkPlay", "0");
    swprintf_s(out, MAX_PATH, L"%s\\%s", g_modDir, folder);
    CreateDirectoryW(out, nullptr);
    wcscat_s(out, MAX_PATH, L"\\session_fguy.ini");
    HANDLE f = CreateFileW(out, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    DWORD put;
    WriteFile(f, ini.data(), (DWORD)ini.size(), &put, nullptr);
    CloseHandle(f);
    return true;
}

void InitConfigOverride(HMODULE exe) {
    wchar_t iniName[MAX_PATH] = L"fguy.ini";
    wchar_t env[MAX_PATH];
    DWORD envLen = GetEnvironmentVariableW(L"FGUYMOD_INI", env, MAX_PATH);
    if (envLen > 0 && envLen < MAX_PATH) wcscpy_s(iniName, env);
    swprintf_s(g_iniPath, L"%s\\%s", g_modDir, iniName);
    g_iniOverride = FileExists(g_iniPath);
    wchar_t saveDir[64];
    DWORD saveLen = GetEnvironmentVariableW(L"FGUYMOD_SAVEDIR", saveDir, 64);
    if (saveLen > 0 && saveLen < 64) {
        swprintf_s(g_saveDir, L"%s\\%s", g_modDir, saveDir);
        CreateDirectoryW(g_saveDir, nullptr);
        Log(L"Config: test run - saves go to %s", g_saveDir);
    }
    wchar_t session[8];
    bool autoBoot = GetEnvironmentVariableW(L"FGUYMOD_SESSION", session, 8) ||
                    GetEnvironmentVariableW(L"FGUYMOD_TEST_AUTOMENU", session, 8);
    if (envLen == 0 && saveLen > 0 && saveLen < 64 && autoBoot && MakeSessionIni(exe, saveDir, g_iniPath))
        g_iniOverride = true;
    Log(g_iniOverride ? L"Config override ON: %s" : L"Config override off (no %s)", g_iniPath);

    HookSpec hooks[] = {
        {"kernel32.dll", "FindResourceW", (void*)Hook_FindResourceW, (void**)&Real_FindResourceW},
        {"kernel32.dll", "CreateFileW", (void*)Hook_CreateFileW, (void**)&Real_CreateFileW},
        {"kernel32.dll", "CreateFileA", (void*)Hook_CreateFileA, (void**)&Real_CreateFileA},
    };
    InstallHooks(exe, hooks);
}
