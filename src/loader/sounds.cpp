// Loading-screen music for online games: a short Family Guy sting while a level loads.
//
// Files (FGUYMod\sounds, made by tools/make_sounds.py): opening.wav and strings_01.wav, strings_02.wav, ...
//   * story level 1 (Frat Row) always gets the opening;
//   * so does the first multiplayer match since the game started;
//   * every other load gets one of the strings, never the same one twice in a row.
// Online only (offline story and multiplayer keep the game's own loading screens): the lobby's loading
// screen plays it for the first match (the session game itself boots hidden and muted, handoff.cpp), and
// inside a session every later level load (the game opening .\FGUY\Levels\...\<level>.ho, config.cpp).
// It plays at the game's Music Volume (audio.cpp). Missing files just mean no music.

#include "common.h"

#include <mmsystem.h>

#include <algorithm>
#include <cstring>
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

std::vector<char> g_playing;      // the sound PlaySound is playing from (kept until the next one)

// The file with its samples at `volume` (16-bit PCM; anything else plays as it is).
bool LoadScaled(const std::wstring& file, float volume, std::vector<char>& out) {
    HANDLE f = CreateFileW(file.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    DWORD size = GetFileSize(f, nullptr), got = 0;
    out.resize(size);
    bool ok = size > 44 && ReadFile(f, out.data(), size, &got, nullptr) && got == size;
    CloseHandle(f);
    if (!ok || memcmp(out.data(), "RIFF", 4) != 0) return false;
    unsigned short bits = 0, format = 0;
    for (size_t at = 12; at + 8 <= out.size();) {
        unsigned len;
        memcpy(&len, &out[at + 4], 4);
        if (memcmp(&out[at], "fmt ", 4) == 0 && len >= 16) {
            memcpy(&format, &out[at + 8], 2);
            memcpy(&bits, &out[at + 22], 2);
        } else if (memcmp(&out[at], "data", 4) == 0 && format == 1 && bits == 16) {
            size_t n = std::min<size_t>(len, out.size() - at - 8) / 2;
            auto s = reinterpret_cast<short*>(&out[at + 8]);
            for (size_t i = 0; i < n; ++i) s[i] = (short)(s[i] * volume);
            break;
        }
        at += 8 + (size_t)len + (len & 1);
    }
    return true;
}

void Play(const std::wstring& file, const wchar_t* why) {
    if (file.empty()) return;
    if (HandoffHidden()) return;  // a session booting hidden: the lobby's loading screen plays it
    float volume = AudioMusicVolume();
    const wchar_t* name = wcsrchr(file.c_str(), L'\\');
    if (volume <= 0.001f) {
        Log(L"Sounds: %s - music volume is off", why);
        return;
    }
    PlaySoundW(nullptr, nullptr, 0);  // stop the last one before its samples go
    if (!LoadScaled(file, volume, g_playing)) return;
    PlaySoundW(reinterpret_cast<LPCWSTR>(g_playing.data()), nullptr, SND_MEMORY | SND_ASYNC | SND_NODEFAULT);
    Log(L"Sounds: %s - %s at %.0f%%", why, name ? name + 1 : file.c_str(), volume * 100);
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
    if (!NetplayEnabled()) return;  // offline: the game's own loading screens
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
