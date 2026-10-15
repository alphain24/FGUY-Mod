// Mods: separate mods next to the base mod, each in FGUYMod\mods\<folder>\ (see sdk\fguymod_plugin.h for the format).
//
// * Which are on: FGUYMod\mods.ini [Enabled] <folder>=1. New mods start off; the MODS page of the main menu switches
//   them (ModsList / ModsToggle) - it takes a restart (plugins and file replacements load at start).
// * A mod's files\<path> is used instead of the game's <path> (config.cpp's file hooks ask ModsFileOverride; the
//   last mod switched on wins). Its ui\*.fgpatch patch the game's movies like the base mod's (ui.cpp: after the base
//   mod's own, on top of them).
// * Its plugin DLL is loaded and given the API (FGUYModInit). OnTick callbacks run at the start of every game tick on
//   the game's thread (vpad.cpp's first controller poll of the tick: online that's right after the lockstep has the
//   tick's inputs), OnFrame callbacks once per drawn frame (d3d9.cpp).
// * Online: the gameplay mods that are on, with a hash of their files (ModsOnlineKey), go to the lobby server, which
//   lets players into a lobby only with the host's (cosmetic mods may differ).

#include "common.h"

#include <intrin.h>
#include <shellapi.h>
#include <xinput.h>

#include <algorithm>
#include <string>
#include <vector>

#include "../../sdk/fguymod_plugin.h"

#pragma intrinsic(_ReturnAddress)

namespace {

struct Mod {
    std::wstring folder, dir, name, version, author, description, plugin;
    bool gameplay = true;      // Type=gameplay (default) / cosmetic
    bool enabled = false;
    bool loaded = false;       // its plugin is in
    HMODULE dll = nullptr;
    FGUYModAPI api{};
    unsigned hash = 0;         // its files (mod.ini, plugin, ui, files), for the online check
};
std::vector<Mod> g_mods;
DWORD g_gameThread = 0;
std::vector<std::pair<FGUYModCallback, void*>> g_onTick, g_onFrame;
unsigned g_offlineTick = 0;
int g_offlineRate = 60;        // offline: game ticks per second, measured (the game runs as fast as it draws)
bool g_changed = false;        // switched on the MODS page since start (a restart applies it)

std::wstring ModsDir() { return std::wstring(g_modDir) + L"\\mods"; }
std::wstring EnabledIni() { return std::wstring(g_modDir) + L"\\mods.ini"; }

std::wstring Ini(const std::wstring& file, const wchar_t* section, const wchar_t* key, const wchar_t* def = L"") {
    wchar_t buf[512];
    GetPrivateProfileStringW(section, key, def, buf, 512, file.c_str());
    return buf;
}

std::string Utf8(const std::wstring& w) {
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s(n, 0);
    if (n) WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), &s[0], n, nullptr, nullptr);
    return s;
}

std::string Clean(const std::wstring& w) {   // (for the menu's '|' / '\n' separated lists)
    std::string s = Utf8(w);
    for (char& c : s)
        if (c == '|' || c == '\n' || c == '\r' || c == ';') c = ' ';
    return s;
}

// FNV-1a over a file's bytes (and its name), into h.
void HashFile(unsigned& h, const std::wstring& path, const std::wstring& name) {
    for (wchar_t c : name) h = (h ^ (unsigned)towlower(c)) * 16777619u;
    HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return;
    std::vector<unsigned char> buf(1 << 16);
    DWORD got = 0;
    while (ReadFile(f, buf.data(), (DWORD)buf.size(), &got, nullptr) && got)
        for (DWORD i = 0; i < got; ++i) h = (h ^ buf[i]) * 16777619u;
    CloseHandle(f);
}

void HashTree(unsigned& h, const std::wstring& dir, const std::wstring& rel) {
    WIN32_FIND_DATAW fd;
    HANDLE f = FindFirstFileW((dir + L"\\*").c_str(), &fd);
    if (f == INVALID_HANDLE_VALUE) return;
    std::vector<std::wstring> names;
    do {
        if (wcscmp(fd.cFileName, L".") && wcscmp(fd.cFileName, L"..")) names.push_back(fd.cFileName);
    } while (FindNextFileW(f, &fd));
    FindClose(f);
    std::sort(names.begin(), names.end());   // (the same order on every PC)
    for (const auto& n : names) {
        std::wstring p = dir + L"\\" + n;
        if (GetFileAttributesW(p.c_str()) & FILE_ATTRIBUTE_DIRECTORY) HashTree(h, p, rel + n + L"\\");
        else HashFile(h, p, rel + n);
    }
}

Mod* ModOfCaller(void* ret) {
    HMODULE m = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCWSTR>(ret), &m);
    for (auto& mod : g_mods)
        if (mod.dll && mod.dll == m) return &mod;
    return nullptr;
}

// ---- the API ---------------------------------------------------------------------------------------------------------
void __cdecl ApiLog(const wchar_t* text) {
    Mod* m = ModOfCaller(_ReturnAddress());
    Log(L"Mod %s: %s", m ? m->name.c_str() : L"?", text ? text : L"");
}
int __cdecl ApiSetting(const wchar_t* key, int def) {
    Mod* m = ModOfCaller(_ReturnAddress());
    if (!m || !key) return def;
    return (int)GetPrivateProfileIntW(L"Settings", key, def, (m->dir + L"\\mod.ini").c_str());
}
int __cdecl ApiIsOnline() { return NetplayEnabled() ? 1 : 0; }
int __cdecl ApiLocalPlayer() { return NetplayEnabled() ? NetplayLocalSlot() : 0; }
unsigned __cdecl ApiTick() { return NetplayEnabled() ? NetplayTick() : g_offlineTick; }
int __cdecl ApiTickRate() { return NetplayEnabled() ? NetplayRate() : g_offlineRate; }
int __cdecl ApiGetPlayers(void** out, int max) {
    unsigned char* list[8];
    int n = GamePlayers(list, max < 8 ? max : 8);
    for (int i = 0; i < n; ++i) out[i] = list[i];
    return n;
}
int __cdecl ApiPlayerIndex(void* p) { return p ? *reinterpret_cast<int*>(static_cast<unsigned char*>(p) + 0xdf8) : -1; }
float* __cdecl ApiPlayerPosition(void* p) {
    if (!p) return nullptr;
    auto xf = *reinterpret_cast<unsigned char**>(static_cast<unsigned char*>(p) + 0x38);
    return xf && !IsBadReadPtr(xf + 0x30, 12) ? reinterpret_cast<float*>(xf + 0x30) : nullptr;
}
float __cdecl ApiPlayerHealth(void* p) { return p ? NetplayPlayerHealth(static_cast<unsigned char*>(p)) : 0.0f; }
int __cdecl ApiGetInput(int slot, FGUYModInput* out) {
    if (!out) return 0;
    ZeroMemory(out, sizeof(*out));
    WORD buttons = 0;
    BYTE lt = 0, rt = 0;
    SHORT sticks[4] = {};
    KbmRaw kbm{};
    bool keyboard = false;
    if (NetplayEnabled()) {
        if (!NetplayInputOf(slot, &buttons, &lt, &rt, sticks, &kbm, &keyboard)) return 0;
    } else {   // offline: this PC's controller and keyboard (nothing to keep in sync)
        if (slot != 0) return 0;
        XINPUT_STATE st{};
        using Get_t = DWORD(WINAPI*)(DWORD, XINPUT_STATE*);
        static Get_t get = nullptr;
        if (!get) if (HMODULE x = GetModuleHandleW(L"xinput1_3.dll")) get = reinterpret_cast<Get_t>(GetProcAddress(x, MAKEINTRESOURCEA(2)));
        if (get && get(0, &st) == ERROR_SUCCESS) {
            buttons = st.Gamepad.wButtons;
            lt = st.Gamepad.bLeftTrigger;
            rt = st.Gamepad.bRightTrigger;
            sticks[0] = st.Gamepad.sThumbLX; sticks[1] = st.Gamepad.sThumbLY;
            sticks[2] = st.Gamepad.sThumbRX; sticks[3] = st.Gamepad.sThumbRY;
        }
        HWND fg = GetForegroundWindow();
        DWORD pid = 0;
        GetWindowThreadProcessId(fg, &pid);
        if (pid == GetCurrentProcessId())
            for (int vk = 1; vk < 256; ++vk)
                if (GetAsyncKeyState(vk) & 0x8000) kbm.keys[vk >> 3] |= (BYTE)(1 << (vk & 7));
        keyboard = true;
    }
    out->buttons = buttons;
    out->leftTrigger = lt;
    out->rightTrigger = rt;
    out->leftX = sticks[0]; out->leftY = sticks[1]; out->rightX = sticks[2]; out->rightY = sticks[3];
    out->keyboardPlayer = keyboard ? 1 : 0;
    memcpy(out->keys, kbm.keys, 32);
    out->mouseX = kbm.dx;
    out->mouseY = kbm.dy;
    out->mouseWheel = kbm.dz;
    out->mouseButtons = kbm.buttons;
    return 1;
}
int __cdecl ApiOnTick(FGUYModCallback fn, void* user) {
    if (!fn) return 0;
    g_onTick.push_back({fn, user});
    return 1;
}
int __cdecl ApiOnFrame(FGUYModCallback fn, void* user) {
    if (!fn) return 0;
    g_onFrame.push_back({fn, user});
    return 1;
}
int __cdecl ApiPatch(void* at, const void* bytes, int n) {
    if (!at || !bytes || n <= 0) return 0;
    DWORD old;
    if (!VirtualProtect(at, n, PAGE_EXECUTE_READWRITE, &old)) return 0;
    memcpy(at, bytes, n);
    VirtualProtect(at, n, old, &old);
    FlushInstructionCache(GetCurrentProcess(), at, n);
    return 1;
}
int __cdecl ApiHook(void* target, int length, void* detour, void** original) {
    if (!target || !detour || length < 5 || length > 32) return 0;
    auto tramp = static_cast<unsigned char*>(VirtualAlloc(nullptr, length + 5, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
    if (!tramp) return 0;
    memcpy(tramp, target, length);   // (whole instructions, none relative: see the SDK)
    tramp[length] = 0xE9;
    *reinterpret_cast<int*>(tramp + length + 1) =
        (int)(reinterpret_cast<uintptr_t>(target) + length - (reinterpret_cast<uintptr_t>(tramp) + length + 5));
    unsigned char jump[32];
    jump[0] = 0xE9;
    *reinterpret_cast<int*>(jump + 1) = (int)(reinterpret_cast<uintptr_t>(detour) - (reinterpret_cast<uintptr_t>(target) + 5));
    for (int i = 5; i < length; ++i) jump[i] = 0x90;
    if (original) *original = tramp;
    return ApiPatch(target, jump, length);
}

void FillApi(Mod& m) {
    FGUYModAPI& a = m.api;
    a.version = FGUYMOD_API_VERSION;
    a.modFolder = m.dir.c_str();
    a.Log = ApiLog;
    a.Setting = ApiSetting;
    a.IsOnline = ApiIsOnline;
    a.LocalPlayer = ApiLocalPlayer;
    a.Tick = ApiTick;
    a.TickRate = ApiTickRate;
    a.GetPlayers = ApiGetPlayers;
    a.PlayerIndex = ApiPlayerIndex;
    a.PlayerPosition = ApiPlayerPosition;
    a.PlayerHealth = ApiPlayerHealth;
    a.GetInput = ApiGetInput;
    a.OnTick = ApiOnTick;
    a.OnFrame = ApiOnFrame;
    a.Hook = ApiHook;
    a.Patch = ApiPatch;
}

}  // namespace

// At start (before the UI's patches and the game's files): the mods there are, and which are on.
void InitMods() {
    g_gameThread = GetCurrentThreadId();
    std::wstring dir = ModsDir();
    CreateDirectoryW(dir.c_str(), nullptr);
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((dir + L"\\*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || fd.cFileName[0] == L'.') continue;
        Mod m;
        m.folder = fd.cFileName;
        m.dir = dir + L"\\" + m.folder;
        std::wstring ini = m.dir + L"\\mod.ini";
        if (!FileExists(ini.c_str())) continue;
        m.name = Ini(ini, L"Mod", L"Name", m.folder.c_str());
        m.version = Ini(ini, L"Mod", L"Version", L"1");
        m.author = Ini(ini, L"Mod", L"Author");
        m.description = Ini(ini, L"Mod", L"Description");
        m.plugin = Ini(ini, L"Mod", L"Plugin");
        m.gameplay = _wcsicmp(Ini(ini, L"Mod", L"Type", L"gameplay").c_str(), L"cosmetic") != 0;
        m.enabled = GetPrivateProfileIntW(L"Enabled", m.folder.c_str(), 0, EnabledIni().c_str()) != 0;
        wchar_t none[4];   // (test knob: FGUYMOD_TEST_NOMODS=1 - this copy runs with every mod off)
        if (GetEnvironmentVariableW(L"FGUYMOD_TEST_NOMODS", none, 4)) m.enabled = false;
        g_mods.push_back(m);
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    std::sort(g_mods.begin(), g_mods.end(), [](const Mod& a, const Mod& b) { return _wcsicmp(a.folder.c_str(), b.folder.c_str()) < 0; });
    for (auto& m : g_mods) {
        if (m.enabled) {
            unsigned hash = 2166136261u;
            HashTree(hash, m.dir, L"");
            m.hash = hash;
        }
        Log(L"Mods: %s %s (%s, %s)%s", m.name.c_str(), m.version.c_str(), m.gameplay ? L"gameplay" : L"cosmetic",
            m.enabled ? L"on" : L"off", m.plugin.empty() ? L"" : (L" - plugin " + m.plugin).c_str());
    }
}

// At the end of start-up (the base mod's hooks are in): the plugins of the mods that are on.
void ModsLoadPlugins() {
    for (auto& m : g_mods) {
        if (!m.enabled || m.plugin.empty()) continue;
        std::wstring path = m.dir + L"\\" + m.plugin;
        m.dll = LoadLibraryW(path.c_str());
        if (!m.dll) {
            Log(L"Mods: %s - its plugin %s didn't load (error %lu)", m.name.c_str(), path.c_str(), GetLastError());
            continue;
        }
        auto init = reinterpret_cast<FGUYModInitFn>(GetProcAddress(m.dll, "FGUYModInit"));
        if (!init) {
            Log(L"Mods: %s - %s has no FGUYModInit", m.name.c_str(), m.plugin.c_str());
            continue;
        }
        FillApi(m);
        m.loaded = init(&m.api) == 1;
        Log(L"Mods: %s - plugin %s", m.name.c_str(), m.loaded ? L"started" : L"FAILED to start");
    }
}

// config.cpp, the game opening a file: a mod's own copy instead? (gamePath as the game asked: relative or absolute)
bool ModsFileOverride(const wchar_t* gamePath, wchar_t* out) {
    if (!gamePath || g_mods.empty()) return false;
    const wchar_t* rel = gamePath;
    size_t gl = wcslen(g_gameDir);
    if (_wcsnicmp(rel, g_gameDir, gl) == 0 && (rel[gl] == L'\\' || rel[gl] == L'/')) rel += gl + 1;
    else if (rel[0] == L'.' && (rel[1] == L'\\' || rel[1] == L'/')) rel += 2;
    else if (rel[0] && rel[1] == L':') return false;   // (somewhere else on the disk)
    std::wstring r = rel;
    std::replace(r.begin(), r.end(), L'/', L'\\');
    for (auto it = g_mods.rbegin(); it != g_mods.rend(); ++it) {
        if (!it->enabled) continue;
        std::wstring p = it->dir + L"\\files\\" + r;
        if (FileExists(p.c_str())) {
            wcsncpy_s(out, MAX_PATH, p.c_str(), _TRUNCATE);
            static int logged = 0;
            if (++logged <= 50) Log(L"Mods: %s uses %s's %s", r.c_str(), it->name.c_str(), p.c_str());
            return true;
        }
    }
    return false;
}

// ui.cpp: the ui folders of the mods that are on (their movie patches), in order.
std::vector<std::wstring> ModsUiFolders() {
    std::vector<std::wstring> out;
    for (auto& m : g_mods)
        if (m.enabled) out.push_back(m.dir + L"\\ui");
    return out;
}

// vpad.cpp: a controller poll - the first of a game tick on the game's thread: the plugins' OnTick.
void ModsTick() {
    if (g_onTick.empty() || GetCurrentThreadId() != g_gameThread) return;
    ++g_offlineTick;
    static ULONGLONG from = 0;
    static unsigned ticks = 0;
    ULONGLONG now = GetTickCount64();
    if (!from) from = now;
    if (++ticks, now - from >= 1000) {   // (the last second's)
        g_offlineRate = (int)(ticks * 1000 / (now - from));
        if (g_offlineRate < 1) g_offlineRate = 1;
        from = now;
        ticks = 0;
    }
    for (auto& cb : g_onTick) cb.first(cb.second);
}

// d3d9.cpp, every frame drawn: the plugins' OnFrame.
void ModsFrame() {
    for (auto& cb : g_onFrame) cb.first(cb.second);
}

// The gameplay mods that are on: "<name> <version>#<hash>;..." (sorted; "" none) - the lobby server matches players by it.
std::string ModsOnlineKey() {
    std::string out;
    for (auto& m : g_mods) {
        if (!m.enabled || !m.gameplay) continue;
        char h[16];
        sprintf_s(h, "#%08x", m.hash);
        auto token = [](std::string s) {   // (letters, digits, _ - . only: the lobby server's rule)
            for (char& c : s)
                if (!isalnum((unsigned char)c) && c != '_' && c != '-' && c != '.') c = '_';
            return s.substr(0, 40);
        };
        out += (out.empty() ? "" : ";") + token(Utf8(m.name)) + "@" + token(Utf8(m.version)) + h;
    }
    return out;
}

// FGUYMOD_ModsList (the MODS page): "<changed 0/1>\n" then a line per mod:
//   <folder>|<name>|<version>|<author>|<type gameplay/cosmetic>|<on 0/1>|<plugin running 0/1>|<description>
const char* ModsList() {
    static std::string out;
    out = std::string(g_changed ? "1" : "0") + "\n";
    for (auto& m : g_mods)
        out += Clean(m.folder) + "|" + Clean(m.name) + "|" + Clean(m.version) + "|" + Clean(m.author) + "|" +
               (m.gameplay ? "gameplay" : "cosmetic") + "|" + (m.enabled ? "1" : "0") + "|" + (m.loaded ? "1" : "0") + "|" +
               Clean(m.description) + "\n";
    return out.c_str();
}

// FGUYMOD_ModsToggle(folder): switch a mod on / off (from the next start). Returns whether it's on now.
bool ModsToggle(const char* folder) {
    if (!folder) return false;
    wchar_t w[MAX_PATH];
    MultiByteToWideChar(CP_UTF8, 0, folder, -1, w, MAX_PATH);
    for (auto& m : g_mods)
        if (_wcsicmp(m.folder.c_str(), w) == 0) {
            m.enabled = !m.enabled;
            WritePrivateProfileStringW(L"Enabled", m.folder.c_str(), m.enabled ? L"1" : L"0", EnabledIni().c_str());
            g_changed = true;
            Log(L"Mods: %s switched %s (from the next start)", m.name.c_str(), m.enabled ? L"on" : L"off");
            return m.enabled;
        }
    return false;
}

// FGUYMOD_ModsOpenFolder: the mods folder in Explorer.
void ModsOpenFolder() {
    std::wstring dir = ModsDir();
    CreateDirectoryW(dir.c_str(), nullptr);
    ShellExecuteW(nullptr, L"open", dir.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}
