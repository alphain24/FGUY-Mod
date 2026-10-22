// Menu (Scaleform) integration.
//
// 1. Movie patches: the game's menus are Scaleform GFX movies stored inside its .ho packages. When
//    the game opens one, its Scaleform::FileOpener returns a GSFMemoryFile pointing into the package.
//    If a patch in FGUYMod\ui\*.fgpatch matches that movie (by length + CRC32), we point the memory
//    file at a patched copy instead. Patches hold only our own bytes (see tools/ui/gfxpatch.py).
//
// 2. Menu calls: menu scripts call into C++ with ExternalInterface.call("CATEGORY_Name", ...). We
//    see every call first; names starting with FGUYMOD_ are ours and never reach the game.
//
// Both are virtual methods on classes with static vtables in FGUY.exe, so we patch the vtable slots.
// Addresses are for the retail PC exe (SHA-256 E4BCD19E...); each slot is checked before patching.

#include "common.h"
#include <algorithm>
#include <intrin.h>

#include <cstdio>
#include <cstring>
#include <cwchar>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <vector>

namespace {

// ---- addresses in FGUY.exe ------------------------------------------------------------------------
const uintptr_t kFileOpenerVtable = 0x9fe670;          // Scaleform::FileOpener
const uintptr_t kFileOpenerOpenFile = 0x4421d0;        //   [1] OpenFile(url, flags, mode)
const uintptr_t kExtHandlerVtable = 0x9fe56c;          // Scaleform::ExternalInterfaceHandler
const uintptr_t kExtHandlerCallback = 0x454190;        //   [1] Callback(movie, name, args, argc)
const uintptr_t kMemoryFileVtable = 0x9fe61c;          // Scaleform::GSFMemoryFile
const uintptr_t kCommandHandlerVtable = 0x9f7668;      // the game's FSCommand handler (menus' getURL("FSCommand:..."))
const uintptr_t kCommandHandlerCallback = 0x5a1620;    //   [0] Callback(movie, "CATG_Name", args)
// GSFMemoryFile fields: +0x08 path, +0x0C data, +0x10 position, +0x14 length.
const int kMemFileData = 0x0C, kMemFilePos = 0x10, kMemFileLen = 0x14;
// GFxMovieView vtable slot used by the game to return a value to the menu script.
const int kSetExternalInterfaceRetVal = 50;

// ---- Scaleform GFxValue (3.x, 32-bit) -------------------------------------------------------------
struct GFxValue {
    void* objectInterface;
    unsigned type;  // low 4 bits: 0 undefined, 1 null, 2 bool, 3 number, 4 string; 0x40 = managed
    union {
        double number;
        bool boolean;
        const char* string;
    };
};
const unsigned kUndefined = 0, kBool = 2, kNumber = 3, kString = 4;

double ArgNumber(const GFxValue* args, unsigned argc, unsigned i) {
    if (i >= argc) return 0;
    switch (args[i].type & 0x0F) {
        case kNumber: return args[i].number;
        case kBool: return args[i].boolean ? 1 : 0;
        case kString: return atof((args[i].type & 0x40) ? *reinterpret_cast<const char* const*>(args[i].string)
                                                           : args[i].string);
        default: return 0;
    }
}

// ---- CRC32 (same as zlib) -------------------------------------------------------------------------
unsigned Crc32(const unsigned char* p, size_t n) {
    static unsigned table[256];
    if (!table[1])
        for (unsigned i = 0; i < 256; ++i) {
            unsigned c = i;
            for (int k = 0; k < 8; ++k) c = c & 1 ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            table[i] = c;
        }
    unsigned c = 0xFFFFFFFFu;
    for (size_t i = 0; i < n; ++i) c = table[(c ^ p[i]) & 0xFF] ^ (c >> 8);
    return c ^ 0xFFFFFFFFu;
}

// ---- .fgpatch files -------------------------------------------------------------------------------
struct Edit {
    unsigned offset, remove;
    std::vector<unsigned char> insert;
};
struct MoviePatch {
    std::wstring name;
    unsigned origLength = 0, origCrc = 0;
    std::vector<Edit> edits;
    std::vector<unsigned char> patched;  // built on first use, then kept for the game's lifetime
};
std::vector<MoviePatch> g_patches;

bool ReadFileBytes(const wchar_t* path, std::vector<unsigned char>& out) {
    FILE* f = nullptr;
    if (_wfopen_s(&f, path, L"rb") || !f) return false;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    out.resize(n > 0 ? n : 0);
    bool ok = n <= 0 || fread(out.data(), 1, n, f) == (size_t)n;
    fclose(f);
    return ok;
}

bool ParsePatch(const std::vector<unsigned char>& b, MoviePatch& p) {
    auto u32 = [&](size_t o) { unsigned v; memcpy(&v, &b[o], 4); return v; };
    if (b.size() < 16 || memcmp(b.data(), "FGP1", 4) != 0) return false;
    p.origLength = u32(4);
    p.origCrc = u32(8);
    unsigned count = u32(12);
    size_t o = 16;
    for (unsigned i = 0; i < count; ++i) {
        if (o + 12 > b.size()) return false;
        Edit e{u32(o), u32(o + 4), {}};
        unsigned n = u32(o + 8);
        o += 12;
        if (o + n > b.size()) return false;
        e.insert.assign(b.begin() + o, b.begin() + o + n);
        o += n;
        p.edits.push_back(std::move(e));
    }
    return true;
}

// The base mod's patches (FGUYMod\\ui), then the mods' that are on (each mods\\<folder>\\ui): a mod's patch made for a
// movie as the base mod patched it applies on top of it (Hook_OpenFile looks again after each patch).
void LoadPatchesFrom(const std::wstring& dir) {
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((dir + L"\\*.fgpatch").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        wchar_t path[MAX_PATH];
        swprintf_s(path, L"%s\\%s", dir.c_str(), fd.cFileName);
        std::vector<unsigned char> bytes;
        MoviePatch p;
        p.name = fd.cFileName;
        if (ReadFileBytes(path, bytes) && ParsePatch(bytes, p)) {
            Log(L"UI: loaded patch %s (%zu edits, for a %u-byte movie)", fd.cFileName, p.edits.size(), p.origLength);
            g_patches.push_back(std::move(p));
        } else {
            Log(L"UI: could not read patch %s", fd.cFileName);
        }
    } while (FindNextFileW(h, &fd));
    FindClose(h);
}

void LoadPatches() {
    LoadPatchesFrom(std::wstring(g_modDir) + L"\\ui");
    for (const auto& dir : ModsUiFolders()) LoadPatchesFrom(dir);
}

bool BuildPatched(MoviePatch& p, const unsigned char* orig) {
    std::vector<unsigned char> out;
    unsigned pos = 0;
    for (const Edit& e : p.edits) {  // edits are sorted and don't overlap
        if (e.offset < pos || e.offset + e.remove > p.origLength) return false;
        out.insert(out.end(), orig + pos, orig + e.offset);
        out.insert(out.end(), e.insert.begin(), e.insert.end());
        pos = e.offset + e.remove;
    }
    out.insert(out.end(), orig + pos, orig + p.origLength);
    p.patched = std::move(out);
    return true;
}

// ---- FileOpener::OpenFile hook ----------------------------------------------------------------------
using OpenFile_t = void*(__thiscall*)(void* self, const char* url, int flags, int mode);
OpenFile_t Real_OpenFile;
std::set<std::string> g_seenFiles;
std::mutex g_openLock;  // movies can be opened by the game's loading threads too

void* __fastcall Hook_OpenFile(void* self, void*, const char* url, int flags, int mode) {
    void* file = Real_OpenFile(self, url, flags, mode);
    if (!file || !url) return file;
    auto bytes = reinterpret_cast<unsigned char*>(file);
    std::lock_guard<std::mutex> lock(g_openLock);
    if (g_seenFiles.insert(url).second || NetplayEnabled())
        Log(L"UI: game opens \"%S\" (file type %#x, %d bytes) - tick %u, thread %lu%s", url,
            *reinterpret_cast<unsigned*>(file), *reinterpret_cast<int*>(bytes + kMemFileLen), NetplayTick(),
            GetCurrentThreadId(), GetCurrentThreadId() == NetplayGameThread() ? L" (game thread)" : L"");
    static int stacks = 0;   // research: who loads the menus' movies
    if (NetplayEnabled() && stacks < 3 && strstr(url, "main_menu")) {
        ++stacks;
        const DWORD* sp = reinterpret_cast<const DWORD*>(_AddressOfReturnAddress());
        const DWORD* top = reinterpret_cast<const DWORD*>(reinterpret_cast<NT_TIB*>(NtCurrentTeb())->StackBase);
        wchar_t line[1024];
        int len = swprintf_s(line, L"UI: movie load callers:");
        for (int i = 0, found = 0; sp + i < top && i < 4000 && found < 40 && len < 980; ++i)
            if (sp[i] >= 0x401000 && sp[i] < 0x9f0000) { len += swprintf_s(line + len, 1024 - len, L" %#lx", sp[i]); ++found; }
        Log(L"%s", line);
    }
    if (*reinterpret_cast<uintptr_t*>(file) != kMemoryFileVtable) return file;
    unsigned char*& data = *reinterpret_cast<unsigned char**>(bytes + kMemFileData);
    int& length = *reinterpret_cast<int*>(bytes + kMemFileLen);
    if (!data || length < 8 || memcmp(data + 1, "FX", 2) != 0) return file;
    // Match on the movie's own size field: packaged files can carry a padding byte after it. After a patch, again
    // (a mod's patch for the movie as patched).
    std::vector<const MoviePatch*> applied;
    for (int round = 0; round < 8; ++round) {
        unsigned movieLength;
        memcpy(&movieLength, data + 4, 4);
        if (movieLength > (unsigned)length) break;
        unsigned crc = 0;
        bool did = false;
        for (MoviePatch& p : g_patches) {
            if (movieLength != p.origLength || std::find(applied.begin(), applied.end(), &p) != applied.end()) continue;
            if (!crc) crc = Crc32(data, movieLength);   // the very movie the patch was made for, every time
            if (crc != p.origCrc) continue;
            if (p.patched.empty() && !BuildPatched(p, data)) continue;
            data = p.patched.data();
            length = (int)p.patched.size();
            *reinterpret_cast<int*>(bytes + kMemFilePos) = 0;
            Log(L"UI: \"%S\" patched with %s", url, p.name.c_str());
            applied.push_back(&p);
            did = true;
            break;
        }
        if (!did) break;
    }
    return file;
}

// ---- ExternalInterfaceHandler::Callback hook -------------------------------------------------------
using Callback_t = void(__thiscall*)(void* self, void* movie, const char* name, const GFxValue* args,
                                     unsigned argc);
Callback_t Real_Callback;
std::set<std::string> g_seenCalls;

void ReturnToMenu(void* movie, const GFxValue& v) {
    using SetRet_t = void(__thiscall*)(void* movie, const GFxValue* value);
    auto vtable = *reinterpret_cast<void***>(movie);
    reinterpret_cast<SetRet_t>(vtable[kSetExternalInterfaceRetVal])(movie, &v);
}
void ReturnNumber(void* movie, double n) { GFxValue v{}; v.type = kNumber; v.number = n; ReturnToMenu(movie, v); }
void ReturnBool(void* movie, bool b) { GFxValue v{}; v.type = kBool; v.boolean = b; ReturnToMenu(movie, v); }
void ReturnString(void* movie, const char* s) { GFxValue v{}; v.type = kString; v.string = s; ReturnToMenu(movie, v); }
// String arguments from the menu are "managed" (type | 0x40): the value then points at the string's
// node, whose first field is the text.
const char* ArgString(const GFxValue* args, unsigned argc, unsigned i) {
    if (i >= argc || (args[i].type & 0x0F) != kString || !args[i].string) return "";
    if (args[i].type & 0x40) {
        auto node = reinterpret_cast<const char* const*>(args[i].string);
        return IsBadReadPtr(node, 4) || !*node ? "" : *node;
    }
    return args[i].string;
}

void __fastcall Hook_Callback(void* self, void*, void* movie, const char* name, const GFxValue* args,
                              unsigned argc) {
    // The supply crates' store in a free-running match (netplay.cpp): someone else's store opens here too (from their
    // presses), but only my own player buys here - theirs is marked bought when their PC says so.
    if (NetplayFreeRun()) {
        int player, weapon;
        while (NetplayStoreTake(&player, &weapon)) {   // (before this call: it may be the weapons wheel asking)
            GFxValue a[2] = {};
            a[0].type = kNumber;
            a[0].number = player;
            a[1].type = kNumber;
            a[1].number = weapon;
            Real_Callback(self, movie, "STOR_SetMultiplayerWeaponBought", a, 2);
            Log(L"UI: player %d's weapon %d marked bought here (bought on their PC)", player, weapon);
        }
        bool buy = name && strcmp(name, "STOR_SetMultiplayerWeaponBought") == 0;
        if (name && (buy || strcmp(name, "STOR_SubtractCurrency") == 0)) {
            player = (int)ArgNumber(args, argc, 0);
            if (!NetplayStoreAllowed(player)) {
                static int logged = 0;
                if (++logged <= 20) Log(L"UI: %S for player %d dropped here (their own PC buys it)", name, player);
                return;
            }
            if (buy) {
                Real_Callback(self, movie, name, args, argc);
                NetplayStoreBought(player, (int)ArgNumber(args, argc, 1));
                return;
            }
        }
    }
    if (name && strncmp(name, "FGUYMOD_", 8) == 0) {
        const char* fn = name + 8;
        static int logged = 0;
        if (logged < 12 && strncmp(fn, "Online", 6) == 0) {
            ++logged;
            Log(L"UI: mod call %S(%S) [%u args]", name, ArgString(args, argc, 0), argc);
        }
        if (strcmp(fn, "GetDisplayMode") == 0) {
            ReturnNumber(movie, DisplayMenuIndex());
        } else if (strcmp(fn, "SetDisplayMode") == 0) {
            ReturnBool(movie, DisplayMenuSelect((int)ArgNumber(args, argc, 0)));
        } else if (strcmp(fn, "ApplyDisplayMode") == 0) {
            DisplayMenuApply();
        } else if (strcmp(fn, "GetFrameRate") == 0) {
            ReturnNumber(movie, VideoFrameRateIndex());
        } else if (strcmp(fn, "SetFrameRate") == 0) {
            VideoSetFrameRate((int)ArgNumber(args, argc, 0));
        } else if (strcmp(fn, "Log") == 0) {  // menu script debugging
            Log(L"UI: script: %S", ArgString(args, argc, 0));
        } else if (strcmp(fn, "HudDebug") == 0) {  // research: make_hud_patch.py --debug
            static std::set<std::string> seen;
            char line[512];
            int len = sprintf_s(line, "%s:", ArgString(args, argc, 0));
            for (int i = 1; i < (int)argc && i < 10 && len < 480; ++i) len += sprintf_s(line + len, 512 - len, " %g", ArgNumber(args, argc, i));
            if (seen.size() < 600 && seen.insert(line).second) Log(L"UI: HUD %S", line);
        } else if (strcmp(fn, "HudSplitMode") == 0) {  // the level's HUD lays itself out: the level runs
            HandoffMatchRunning();
            SplitViewHudHas4Way(argc < 2 || ArgNumber(args, argc, 1) != 0);   // (mode, has a 4-way layout)
            Log(L"UI: HUD split mode %d -> %d", (int)ArgNumber(args, argc, 0), SplitViewHudMode((int)ArgNumber(args, argc, 0)));
            ReturnNumber(movie, SplitViewHudMode((int)ArgNumber(args, argc, 0)));
        } else if (strcmp(fn, "ResultsLeave") == 0) {   // the results menu: go back to the lobby now? (netplay.cpp)
            ReturnBool(movie, NetplayResultsLeave());
        } else if (strcmp(fn, "RematchPending") == 0) {   // the lobby: back for the next match? (online.cpp)
            ReturnString(movie, OnlineRematchPending());
        } else if (strcmp(fn, "RematchDone") == 0) {
            OnlineRematchDone();
        } else if (strcmp(fn, "KeyPlayer") == 0) {   // (key code): whose key it is - online keyboard players (kbm.cpp)
            ReturnNumber(movie, KbmKeyPlayer((int)ArgNumber(args, argc, 0)));
        } else if (strcmp(fn, "HudZoom") == 0) {   // online: which quarter of the 4-way HUD is mine (-1: none)
            ReturnNumber(movie, SplitViewHudZoomViewport());
        } else if (strcmp(fn, "HudViewport") == 0) {
            int v = SplitViewHudViewport((int)ArgNumber(args, argc, 0));
            static std::set<int> hudSeen;
            int in = (int)ArgNumber(args, argc, 0);
            if (hudSeen.insert(in * 16 + v).second) Log(L"UI: HUD viewport %d -> %d", in, v);
            ReturnNumber(movie, v);
        } else if (strcmp(fn, "OnlineEnter") == 0) {
            OnlineEnter();
        } else if (strcmp(fn, "OnlineExit") == 0) {
            OnlineExit();
        } else if (strcmp(fn, "LobbyState") == 0) {
            ReturnString(movie, LobbyState());
        } else if (strcmp(fn, "LobbyDo") == 0) {
            LobbyDo(ArgString(args, argc, 0), ArgString(args, argc, 1), ArgString(args, argc, 2),
                    ArgString(args, argc, 3), ArgString(args, argc, 4), ArgString(args, argc, 5));
        } else if (strcmp(fn, "SessionConfig") == 0) {
            ReturnString(movie, SessionConfig());
        } else if (strcmp(fn, "SessionPress") == 0) {  // (pad, button name, delay in ticks)
            static const struct { const char* name; WORD bit; } buttons[] = {
                {"a", 0x1000}, {"b", 0x2000}, {"x", 0x4000}, {"y", 0x8000}, {"start", 0x0010}, {"back", 0x0020},
                {"up", 0x0001}, {"down", 0x0002}, {"left", 0x0004}, {"right", 0x0008}};
            WORD bit = 0;
            for (auto& b : buttons)
                if (_stricmp(ArgString(args, argc, 1), b.name) == 0) bit = b.bit;
            if (NetplayEnabled()) NetplaySessionPress((int)ArgNumber(args, argc, 0), bit, (int)ArgNumber(args, argc, 2));
            else VPadPressLater((DWORD)ArgNumber(args, argc, 0), bit, (DWORD)(ArgNumber(args, argc, 2) * 1000 / 60));
        } else if (strcmp(fn, "TestAutoMenu") == 0) {  // test copies: walk to a menu page by themselves
            static char v[32];
            if (!GetEnvironmentVariableA("FGUYMOD_TEST_AUTOMENU", v, sizeof(v))) v[0] = 0;
            ReturnString(movie, v);
        } else if (strcmp(fn, "TitleScreen") == 0) {   // the title screen shows (start patch)
            AudioTitleScreen();
        } else if (strcmp(fn, "ProfileLoaded") == 0) {   // the main menu opened (menu patch, OnHomeEnter)
            AudioProfileLoaded();
            OnlineSettingsProfileLoaded();   // (a session game: my own sound over the host's save)
        } else if (strcmp(fn, "MySettings") == 0) {      // (the lobby: the game's own values) for the next match
            OnlineSettingsReport(ArgString(args, argc, 0));
        } else if (strcmp(fn, "SettingsBack") == 0) {    // back from a match: what changed there, for the setters
            ReturnString(movie, OnlineSettingsBack());
        } else if (strcmp(fn, "PauseMenu") == 0) {   // the online pause menu (the HUD, every frame)
            ReturnString(movie, OnlinePauseMenu());
        } else if (strcmp(fn, "PauseCursor") == 0) {
            ReturnString(movie, OnlinePauseCursor());
        } else if (strcmp(fn, "HudTakesStart") == 0) {   // (1 / 0): my weapons screen is up - START is its own
            OnlinePauseHudTakesStart(ArgNumber(args, argc, 0) != 0);
        } else if (strcmp(fn, "DeathPanel") == 0) {   // (my death menu's panel up / gone)
            SplitViewNoMarkers(ArgNumber(args, argc, 0) != 0);
            ReturnBool(movie, true);
        } else if (strcmp(fn, "DeathMouse") == 0) {   // (my death menu's buttons) -> the pointer
            ReturnString(movie, OnlinePauseDeathMouse(ArgString(args, argc, 0)));
        } else if (strcmp(fn, "MenuMouse") == 0) {    // (the end of the match's menu) -> the Windows pointer shown?
            ReturnString(movie, OnlineMenuMouse(ArgString(args, argc, 0), ArgString(args, argc, 1)));
        } else if (strcmp(fn, "Rematch") == 0) {   // (the end of the match's menu: the rematch vote)
            ReturnString(movie, OnlinePauseRematch());
        } else if (strcmp(fn, "PauseSettings") == 0) {   // (the game's pause menu: OPTIONS online -> my own settings)
            OnlinePauseSettings();
            ReturnBool(movie, true);
        } else if (strcmp(fn, "ChatMode") == 0) {        // (the lobby's chat box: typing in it, or not)
            OnlineMenuChatMode(ArgNumber(args, argc, 0) != 0);
            ReturnBool(movie, true);
        } else if (strcmp(fn, "ChatTyped") == 0) {       // (what was typed into it since)
            ReturnString(movie, OnlineMenuChatTyped());
        } else if (strcmp(fn, "UpdateState") == 0) {     // (the ONLINE screen: is there a newer version?)
            ReturnString(movie, UpdateState());
        } else if (strcmp(fn, "UpdateInstall") == 0) {   // (install it)
            UpdateInstall();
            ReturnBool(movie, true);
        } else if (strcmp(fn, "ModsList") == 0) {        // (the MODS page: the mods there are)
            ReturnString(movie, ModsList());
        } else if (strcmp(fn, "ModsToggle") == 0) {      // (switch one on / off - from the next start)
            ReturnBool(movie, ModsToggle(ArgString(args, argc, 0)));
        } else if (strcmp(fn, "ModsOpenFolder") == 0) {  // (the mods folder in Explorer)
            ModsOpenFolder();
            ReturnBool(movie, true);
        } else if (strcmp(fn, "ReportCount") == 0) {     // (the ONLINE screen: reports saved)
            ReturnNumber(movie, ReportCount());
        } else if (strcmp(fn, "ReportNew") == 0) {       // (back from a match: one saved in it?)
            ReturnString(movie, ReportNew());
        } else if (strcmp(fn, "OpenReports") == 0) {     // (the REPORTS button: the folder)
            ReportOpenFolder();
            ReturnBool(movie, true);
        } else if (strcmp(fn, "Spectating") == 0) {      // (the HUD: whose view this PC shows, "" mine)
            static char who[64];
            WideCharToMultiByte(CP_UTF8, 0, SplitViewSpectating(), -1, who, sizeof(who), nullptr, nullptr);
            ReturnString(movie, who);
        } else if (strcmp(fn, "NetStats") == 0) {        // (the HUD: the other players' pings, top right)
            static std::string stats;
            stats = OnlineShowPing() ? NetplayStats() : std::string();
            ReturnString(movie, stats.c_str());
        } else if (strcmp(fn, "StoryCoop") == 0) {       // (the game's pause menu: an online story co-op?)
            ReturnBool(movie, NetplayStoryCoop());
        } else if (strcmp(fn, "MyPlayer") == 0) {     // online: my player's index (-1: not yet)
            ReturnNumber(movie, NetplayEnabled() ? NetplayLocalSlot() : -1);
        } else if (strcmp(fn, "PauseQuitDone") == 0) {
            OnlinePauseQuitDone();
        } else if (strcmp(fn, "TestHudDump") == 0) {   // research: the HUD logs where its elements are (pause.as2js)
            ReturnBool(movie, SettingInt(L"Debug", L"HudDump", L"FGUYMOD_TEST_HUDDUMP", 0) != 0);
        } else if (strcmp(fn, "TestAnnounce") == 0) {   // test tools: the HUD shows its "ready" announcement
            ReturnNumber(movie, SettingInt(L"Debug", L"TestAnnounce", L"FGUYMOD_TEST_ANNOUNCE", 0));
        } else if (strcmp(fn, "TestShowDamageFx") == 0) {   // test tools: the HUD shows its damage frame (screenshots)
            ReturnBool(movie, SettingInt(L"Debug", L"ShowDamageFx", L"FGUYMOD_TEST_DAMAGEFX", 0) != 0);
        } else if (strcmp(fn, "Now") == 0) {   // real milliseconds (a match copy's getTimer() is off: the fixed clock)
            ReturnNumber(movie, (double)GetTickCount64());
        } else if (strcmp(fn, "OnlineMatch") == 0) {   // the game's pause menu (pause patch): an online match?
            ReturnBool(movie, NetplayEnabled());
        } else if (strcmp(fn, "HudShown") == 0) {   // the level's HUD (pause.as2js): showing? (frame rate: d3d9.cpp)
            VideoHudShown(ArgNumber(args, argc, 0) != 0);
            ReturnBool(movie, true);
        } else if (strcmp(fn, "KeyboardPlayer") == 0) {   // online HUD: does this player play with keyboard and mouse?
            bool given = argc > 0 && (args[0].type & 0x0F) == kNumber;   // (none given: the game means its first player -
            int player = given ? (int)ArgNumber(args, argc, 0) : 0;      //  as offline)
            ReturnBool(movie, KbmIsKeyboardSlot(player));
        } else if (strcmp(fn, "SessionHandBack") == 0) {   // 0: the lobby opens here, 1 / 2: the lobby game takes over
            ReturnNumber(movie, HandoffGiveBackState());
        } else if (strcmp(fn, "SessionKeyboard") == 0) {   // back from a match: did I play on keyboard / mouse?
            ReturnBool(movie, KbmIsKeyboardSlot(NetplaySessionSlot()));
        } else if (strcmp(fn, "SessionControls") == 0) {   // (keyboard?) after the game's own switch to them
            KbmBackInLobby(ArgNumber(args, argc, 0) != 0);
        } else if (strcmp(fn, "SessionReturn") == 0) {   // a front-end page opened: is the match over?
            ReturnBool(movie, NetplaySessionReturn());
        } else if (strcmp(fn, "SetMovieVolume") == 0) {   // Options > Audio > MOVIES & TITLE
            AudioSetMovieVolume((float)ArgNumber(args, argc, 0));
        } else if (strcmp(fn, "CoverViewports") == 0) {  // cover.gfx: online it's one full-screen view
            if (NetplayEnabled()) ReturnNumber(movie, 1);    //   (offline: no answer, the game's own count)
        } else if (strcmp(fn, "TypedText") == 0) {  // letters / digits / \b typed on the ONLINE screens
            ReturnString(movie, OnlineMenuTyped());
        } else if (strcmp(fn, "PadConnected") == 0) {   // a controller is plugged in (any of Windows' 4 slots)
            // (test copies: their virtual pad - unless FGUYMOD_TEST_NOPAD: as if none were plugged in)
            bool testPad = SettingInt(L"Debug", L"TestInput", L"FGUYMOD_TESTINPUT", 0) != 0 &&
                           !SettingInt(L"Debug", L"_", L"FGUYMOD_TEST_NOPAD", 0);
            ReturnBool(movie, testPad || (VPadRealPadConnected() && !SettingInt(L"Debug", L"_", L"FGUYMOD_TEST_NOPAD", 0)));
        } else if (strcmp(fn, "FromWASD") == 0) {   // (direction key): it came from W A S D
            ReturnBool(movie, OnlineMenuFromWASD((int)ArgNumber(args, argc, 0)));
        } else if (strcmp(fn, "TurnPreview") == 0) {  // (radians): turn the 3D character preview
            OnlineMenuTurnPreview((float)ArgNumber(args, argc, 0));
        } else if (strcmp(fn, "IsBot") == 0) {
            ReturnBool(movie, IsBot());
        } else if (strcmp(fn, "BotTeam") == 0) {   // test: the bot's team (FGUYMOD_BOT_TEAM 0 Good / 1 Evil; -1: at random)
            char team[8] = "";
            ReturnNumber(movie, GetEnvironmentVariableA("FGUYMOD_BOT_TEAM", team, sizeof(team)) ? atoi(team) : -1);
        } else if (strcmp(fn, "TestChar") == 0) {   // test: who I play in online lobbies (FGUYMOD_TEST_CHAR=Brian; "")
            static char who[32] = "";
            if (!GetEnvironmentVariableA("FGUYMOD_TEST_CHAR", who, sizeof(who))) who[0] = 0;
            ReturnString(movie, who);
        } else if (strcmp(fn, "BotJoin") == 0) {
            ReturnString(movie, BotJoinCode());
        } else if (strcmp(fn, "LoadingSound") == 0) {  // (mode, map): the lobby shows its loading screen
            SoundsOnlineLoading(ArgString(args, argc, 0), ArgString(args, argc, 1));
        } else if (strcmp(fn, "PadPressed") == 0) {  // (button bits): pressed on a controller just now?
            ReturnBool(movie, VPadPressedWithin((WORD)ArgNumber(args, argc, 0), 250));
        } else if (strcmp(fn, "MenuView") == 0) {  // (left, top, right, bottom) of the screen, 0..1
            SplitViewMenuRect((float)ArgNumber(args, argc, 0), (float)ArgNumber(args, argc, 1),
                              (float)ArgNumber(args, argc, 2), (float)ArgNumber(args, argc, 3));
        } else if (strcmp(fn, "MenuCamera") == 0) {   // the menus' 3D camera: "x|y|z"
            ReturnString(movie, SplitViewMenuCamera());
        } else if (strcmp(fn, "TestNo3DFix") == 0) {   // test: the lobby's 3D camera check off (to see it fail)
            ReturnBool(movie, SettingInt(L"Debug", L"No3DFix", L"FGUYMOD_TEST_NO3DFIX", 0) != 0);
        } else if (strcmp(fn, "SessionLevel") == 0) {
            NetplaySessionLevel();
        } else if (strcmp(fn, "SessionLog") == 0) {
            Log(L"Session: %S", ArgString(args, argc, 0));
        } else {
            Log(L"UI: unknown mod call %S", name);
        }
        return;
    }
    {   // research (with FGUYMOD_RNGTRACE): every menu call in a session with its tick, into cb_<pid>.txt - two PCs'
        // files side by side show a call one PC made and the other didn't (or later)
        static int on = -1;
        static FILE* f = nullptr;
        if (on < 0) {
            on = SettingInt(L"Debug", L"RngTrace", L"FGUYMOD_RNGTRACE", 0);
            wchar_t path[MAX_PATH];
            swprintf_s(path, L"%s\\cb_%lu.txt", g_modDir, GetCurrentProcessId());
            if (on && _wfopen_s(&f, path, L"w") != 0) f = nullptr;
        }
        if (f && name && NetplayEnabled()) {
            fprintf(f, "%u %s", NetplayTick(), name);
            for (unsigned i = 0; i < argc && i < 4; ++i) {
                if ((args[i].type & 0x0F) == kString) fprintf(f, " '%s'", ArgString(args, argc, i));
                else fprintf(f, " %g", ArgNumber(args, argc, i));
            }
            fputc('\n', f);
            fflush(f);   // (test copies get closed without a goodbye)
        }
    }
    if (name && g_seenCalls.insert(name).second) {
        if (NetplayEnabled()) Log(L"UI: menu calls %S (%u args) - tick %u", name, argc, NetplayTick());
        else Log(L"UI: menu calls %S (%u args)", name, argc);
    }
    // The Screen page reads the real settings when it opens (and again after OK, once our apply has
    // already run), so a choice left over from backing out without OK is dropped here.
    if (name && strcmp(name, "GRFX_IsWindowed") == 0) DisplayMenuReset();
    if (name && strcmp(name, "SNDS_GetMovieVolume") == 0) {   // our MOVIES & TITLE slider (the game has none)
        ReturnNumber(movie, AudioMovieVolume());
        return;
    }
    // Online every PC shows one full-screen view, but the split-screen cover (cover.gfx, the only in-level
    // caller) still lays itself out for two views: side by side it draws a line down the middle of the
    // screen. Top / bottom has none, so online it's always told that.
    if (name && NetplayEnabled() && strcmp(name, "OPTN_GetSplitScreenOrientation") == 0) {
        ReturnNumber(movie, 0);
        return;
    }
    if (name && strcmp(name, "GRFX_ChangeResolution") == 0 && argc >= 2) {
        int w = (int)ArgNumber(args, argc, 0), h = (int)ArgNumber(args, argc, 1);
        if (DisplayOnChangeResolution(&w, &h, ArgNumber(args, argc, 2) != 0)) {
            // borderless: the game gets the monitor's own resolution instead of the one picked
            auto a = const_cast<GFxValue*>(args);
            if ((a[0].type & 0x0F) == kNumber) a[0].number = w;
            if ((a[1].type & 0x0F) == kNumber) a[1].number = h;
        }
    }
    Real_Callback(self, movie, name, args, argc);
}

// ---- menu commands (FSCommand) ---------------------------------------------------------------------
using Command_t = void(__fastcall*)(void* self, void* edx, void* movie, const char* command, const char* args);
Command_t Real_Command = nullptr;

void __fastcall Hook_Command(void* self, void* edx, void* movie, const char* command, const char* args) {
    if (NetplayResultsCommand(command) == 2) {   // (the end of the match's pick, free-running: netplay.cpp)
        Log(L"UI: command %S(%S) - online, back to the lobby for the next match instead (GAME_Quit)", command, args ? args : "");
        Real_Command(self, edx, movie, "GAME_Quit", "");
        return;
    }
    if (command && (strncmp(command, "CTRL_", 5) == 0 || strncmp(command, "GAME_", 5) == 0 || NetplayEnabled())) {
        static int logged = 0;
        if (logged < 200 || NetplayEnabled()) {   // (online: every one, with its tick - the same on every PC)
            ++logged;
            Log(L"UI: command %S(%S), buttons %s%s", command, args ? args : "",
                self && !IsBadReadPtr(static_cast<char*>(self) + 0x60, 1) && static_cast<char*>(self)[0x60] ? L"on" : L"off",
                NetplayEnabled() ? (std::wstring(L" - tick ") + std::to_wstring(NetplayTick())).c_str() : L"");
        }
        // A level's menus (death menu, character / weapon screens) take a controller's input from CTRL_AddInput(n)
        // to CTRL_RemoveInput(n) - the same tick on every PC (kbm.cpp: Enter accepts there, as Start)
        if (NetplayEnabled() && args && (strcmp(command, "CTRL_AddInput") == 0 || strcmp(command, "CTRL_RemoveInput") == 0))
            KbmLevelMenu(atoi(args), command[5] == 'A');
        static int stacks = 0;   // research: from where the menus' commands come (which thread, which call)
        if (NetplayEnabled() && stacks < 2) {
            ++stacks;
            const DWORD* sp = reinterpret_cast<const DWORD*>(_AddressOfReturnAddress());
            const DWORD* top = reinterpret_cast<const DWORD*>(reinterpret_cast<NT_TIB*>(NtCurrentTeb())->StackBase);
            wchar_t line[1024];
            int len = swprintf_s(line, L"UI: command from thread %lu, callers:", GetCurrentThreadId());
            for (int i = 0, found = 0; sp + i < top && i < 3000 && found < 40 && len < 980; ++i)
                if (sp[i] >= 0x401000 && sp[i] < 0x9f0000) { len += swprintf_s(line + len, 1024 - len, L" %#lx", sp[i]); ++found; }
            Log(L"%s", line);
        }
    }
    Real_Command(self, edx, movie, command, args);
    OnlinePauseCommand(command);
}

// ---- vtable patching -------------------------------------------------------------------------------
bool PatchSlot(uintptr_t vtable, int slot, uintptr_t expected, void* hook, void** real, const wchar_t* what) {
    auto entry = reinterpret_cast<uintptr_t*>(vtable + 4 * slot);
    if (IsBadReadPtr(entry, 4) || *entry != expected) {
        Log(L"UI: %s not found at the expected address (different game version?) - skipped", what);
        return false;
    }
    DWORD old;
    VirtualProtect(entry, 4, PAGE_READWRITE, &old);
    *real = reinterpret_cast<void*>(*entry);
    *entry = reinterpret_cast<uintptr_t>(hook);
    VirtualProtect(entry, 4, old, &old);
    Log(L"UI: hooked %s", what);
    return true;
}

}  // namespace

void InitUi() {
    LoadPatches();
    PatchSlot(kFileOpenerVtable, 1, kFileOpenerOpenFile, (void*)Hook_OpenFile, (void**)&Real_OpenFile,
              L"Scaleform::FileOpener::OpenFile");
    PatchSlot(kExtHandlerVtable, 1, kExtHandlerCallback, (void*)Hook_Callback, (void**)&Real_Callback,
              L"Scaleform::ExternalInterfaceHandler::Callback");
    PatchSlot(kCommandHandlerVtable, 0, kCommandHandlerCallback, (void*)Hook_Command, (void**)&Real_Command,
              L"the menus' FSCommand handler");
}
