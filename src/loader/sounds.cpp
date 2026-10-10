// Loading-screen music: a short Family Guy sting while a level loads.
//
// Files (FGUYMod\sounds, made by tools/make_sounds.py): opening.wav and strings_01.wav, strings_02.wav, ...
//   * story level 1 (Frat Row) always gets the opening;
//   * so does the first multiplayer match since the game started;
//   * every other load gets one of the strings, never the same one twice in a row.
// A load is noticed when the game opens the level's package (.\FGUY\Levels\...\<level>.ho, config.cpp),
// and for online matches when the lobby shows its loading screen (the session game itself boots hidden
// and muted, handoff.cpp, so it stays quiet). Missing files just mean no music.

#include "common.h"

#include <mmsystem.h>

#include <cwchar>
#include <string>
#include <vector>

#pragma comment(lib, "winmm.lib")

namespace {

std::wstring g_opening;
std::vector<std::wstring> g_strings;
int g_lastString = -1;
bool g_mpPlayed = false;          // the first multiplayer match has had its opening
wchar_t g_lastLevel[8] = L"";
ULONGLONG g_lastLevelAt = 0;
unsigned g_rand = 0;              // our own generator: the game's random numbers stay untouched

void Play(const std::wstring& file, const wchar_t* why) {
    if (file.empty()) return;
    if (HandoffHidden()) return;  // a session booting hidden: the lobby's loading screen plays it
    PlaySoundW(file.c_str(), nullptr, SND_FILENAME | SND_ASYNC | SND_NODEFAULT);
    const wchar_t* name = wcsrchr(file.c_str(), L'\\');
    Log(L"Sounds: %s - %s", why, name ? name + 1 : file.c_str());
}

void PlayString(const wchar_t* why) {
    if (g_strings.empty()) return Play(g_opening, why);
    int n = (int)g_strings.size(), pick = 0;
    g_rand = g_rand * 1103515245u + 12345u;
    pick = (int)((g_rand >> 16) % (unsigned)n);
    if (n > 1 && pick == g_lastString) pick = (pick + 1) % n;
    g_lastString = pick;
    Play(g_strings[pick], why);
}

// story: "XX" levels; multiplayer: DM, CF, HD, IN; anything else (challenges) gets a string.
void OnLevel(const wchar_t* level, bool story, bool multiplayer, const wchar_t* why) {
    if (story && _wcsnicmp(level, L"FR", 2) == 0) Play(g_opening, why);
    else if (multiplayer && !g_mpPlayed) Play(g_opening, why);
    else PlayString(why);
    if (multiplayer) g_mpPlayed = true;
}

}  // namespace

void InitSounds() {
    wchar_t dir[MAX_PATH], path[MAX_PATH];
    swprintf_s(dir, L"%s\\sounds", g_modDir);
    swprintf_s(path, L"%s\\opening.wav", dir);
    if (FileExists(path)) g_opening = path;
    for (int i = 1; i < 100; ++i) {
        swprintf_s(path, L"%s\\strings_%02d.wav", dir, i);
        if (!FileExists(path)) break;
        g_strings.push_back(path);
    }
    g_rand = (unsigned)GetTickCount64() ^ GetCurrentProcessId();
    Log(L"Sounds: loading music %s (%zu strings)", g_opening.empty() ? L"has no opening.wav" : L"ready",
        g_strings.size());
}

// config.cpp: every file the game opens.
void SoundsOnFileOpen(const wchar_t* path) {
    if (!path || (g_opening.empty() && g_strings.empty())) return;
    const wchar_t* levels = wcsstr(path, L"Levels");
    if (!levels || (levels[6] != L'\\' && levels[6] != L'/')) return;
    const wchar_t* name = wcsrchr(path, L'\\');
    const wchar_t* slash = wcsrchr(path, L'/');
    if (!name || (slash && slash > name)) name = slash;
    name = name ? name + 1 : path;
    if (wcslen(name) != 7 || _wcsicmp(name + 4, L".ho") != 0) return;  // <world><mode>.ho
    wchar_t level[5];
    wcsncpy_s(level, name, 4);
    ULONGLONG now = GetTickCount64();
    if (_wcsicmp(level, g_lastLevel) == 0 && now - g_lastLevelAt < 30000) {  // the game opens it several times
        g_lastLevelAt = now;
        return;
    }
    wcscpy_s(g_lastLevel, level);
    g_lastLevelAt = now;
    const wchar_t* mode = level + 2;
    bool story = _wcsicmp(mode, L"XX") == 0;
    bool mp = !_wcsicmp(mode, L"DM") || !_wcsicmp(mode, L"CF") || !_wcsicmp(mode, L"HD") || !_wcsicmp(mode, L"IN");
    wchar_t why[48];
    swprintf_s(why, L"loading %s", level);
    OnLevel(level, story, mp, why);
}

// The online lobby's loading screen (mode code from the lobby: ST = story, else multiplayer; map = world).
void SoundsOnlineLoading(const char* mode, const char* map) {
    wchar_t level[8];
    swprintf_s(level, L"%.2S%.2S", map ? map : "", mode && strcmp(mode, "ST") == 0 ? "XX" : (mode ? mode : ""));
    bool story = mode && strcmp(mode, "ST") == 0;
    OnLevel(level, story, !story, L"online loading screen");
}
