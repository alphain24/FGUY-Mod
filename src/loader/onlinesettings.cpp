// Every player's own settings in an online match, and the SETTINGS page of the online pause menu.
//
// The session game runs on the host's save, so on its own every player would get the host's settings. Instead:
// * Settings that change how the game plays - aim sensitivity, invert look, reticles - the game keeps per
//   player (sensitivity 0xbad810[list index], reticles 0xbadcd4[list index] and the player's +0x117b8, invert
//   per view at 0xb5e590 + view * 0x3c70). Each player's travel with their input every tick (KbmRaw.aim /
//   .opts), and every PC writes everyone's at the start of that tick: the same values at the same tick on every
//   PC, so the match stays identical - also when someone changes theirs mid-match.
// * Settings only this PC hears or sees - the four volumes, subtitles, vibration, display - apply here only.
// Where they come from: the player's own game (the lobby game) reads them from the game when the ONLINE
// screens open (lobby.as2js -> FGUYMOD_MySettings), hands them to the session game (FGUYMOD_MY_SETTINGS), and
// gets them back after the match (a file the session game writes when it hands back, FGUYMOD_SettingsBack ->
// the lobby applies them through the game's own setters, so the save keeps them).

#include "common.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

namespace {

const uintptr_t kPlayerList = 0xbad988;     // player pointers, count at +0x20
const uintptr_t kAimSensitivity = 0xbad810;  // float per player (list index)
const uintptr_t kReticlesAt = 0xbadcd4;        // byte per player (list index)
const uintptr_t kInvertAt = 0xb5e590;          // byte per view, 0x3c70 apart
const uintptr_t kSubtitles = 0xbad65c;       // int
const int kViewSize = 0x3c70;
using ReticleApply_t = void(__thiscall*)(void* playerPart, int on);   // 0x98f0a0 on player + 0x117b8
using Bool_t = bool(__cdecl*)();                                       // 0x617260 (the menu skips the apply if set)

struct Mine {
    bool known = false;   // from the player's own game
    int aim = 50;         // 0..100
    bool invert = false, reticles = true, subtitles = true;
    float vol[4] = {1, 1, 1, 1};   // music, effects, dialog, cinematics
};
Mine g_mine;
bool g_vibration = true;
std::string g_back;                               // lobby game: what came back from the match

std::string Format(const Mine& m) {
    char buf[200];
    sprintf_s(buf, "aim=%d;inv=%d;ret=%d;sub=%d;mus=%.2f;sfx=%.2f;dlg=%.2f;cin=%.2f", m.aim, m.invert ? 1 : 0,
              m.reticles ? 1 : 0, m.subtitles ? 1 : 0, m.vol[0], m.vol[1], m.vol[2], m.vol[3]);
    return buf;
}

bool Parse(const char* s, Mine* m) {
    if (!s || !*s) return false;
    int fields = 0;
    for (const char* p = s; *p;) {
        const char* end = strchr(p, ';');
        std::string item(p, end ? end - p : strlen(p));
        size_t eq = item.find('=');
        if (eq != std::string::npos) {
            std::string k = item.substr(0, eq);
            double v = atof(item.c_str() + eq + 1);
            ++fields;
            if (k == "aim") m->aim = v < 0 ? 0 : (v > 100 ? 100 : (int)(v + 0.5));
            else if (k == "inv") m->invert = v != 0;
            else if (k == "ret") m->reticles = v != 0;
            else if (k == "sub") m->subtitles = v != 0;
            else if (k == "mus") m->vol[0] = (float)v;
            else if (k == "sfx") m->vol[1] = (float)v;
            else if (k == "dlg") m->vol[2] = (float)v;
            else if (k == "cin") m->vol[3] = (float)v;
            else --fields;
        }
        if (!end) break;
        p = end + 1;
    }
    m->known = fields >= 8;
    return m->known;
}

int Percent(float v) { return (int)(v * 100 + 0.5f); }

void BackFile(DWORD lobbyPid, wchar_t* out) { swprintf_s(out, MAX_PATH, L"%s\\settings_back_%lu.txt", g_modDir, lobbyPid); }

void SaveFlag(const wchar_t* name, bool on) {
    wchar_t path[MAX_PATH];
    swprintf_s(path, L"%s\\FGUYMod.ini", g_modDir);
    WritePrivateProfileStringW(L"Online", name, on ? L"1" : L"0", path);
}

bool g_showPing = true;   // the match shows every player's ping (top right)

void SaveVibration() {
    wchar_t path[MAX_PATH];
    swprintf_s(path, L"%s\\FGUYMod.ini", g_modDir);
    WritePrivateProfileStringW(L"Online", L"Vibration", g_vibration ? L"1" : L"0", path);
}

}  // namespace

bool OnlineVibration() { return g_vibration; }
bool OnlineShowPing() { return g_showPing; }

// The session game, at start: the player's own settings from their lobby game.
void OnlineSettingsInit() {
    g_vibration = SettingInt(L"Online", L"Vibration", nullptr, 1) != 0;
    g_showPing = SettingInt(L"Online", L"ShowPing", nullptr, 1) != 0;
    char env[256];
    if (GetEnvironmentVariableA("FGUYMOD_MY_SETTINGS", env, sizeof(env)) && Parse(env, &g_mine))
        Log(L"Settings: mine for this match: %S", env);
}

// My input for a tick: my aim sensitivity / invert / reticles (once they're known).
void OnlineSettingsStamp(KbmRaw* k) {
    if (!g_mine.known) return;
    k->aim = (BYTE)g_mine.aim;
    k->opts = (BYTE)((g_mine.invert ? 1 : 0) | (g_mine.reticles ? 2 : 0) | 0x80);
}

// Every tick on every PC, before the game runs it: every player's own aim settings onto their player.
void OnlineSettingsApply(const KbmRaw* bySlot, int players) {
    auto list = *reinterpret_cast<unsigned char* volatile*>(kPlayerList);
    if (!list || IsBadReadPtr(list, 0x24)) return;
    int count = *reinterpret_cast<int*>(list + 0x20);
    for (int i = 0; i < count && i < 8; ++i) {
        auto p = reinterpret_cast<unsigned char**>(list)[i];
        if (!p || IsBadReadPtr(p, 0xe00)) continue;
        int slot = *reinterpret_cast<int*>(p + 0xdf8);
        if (slot < 0 || slot >= players || slot >= 4) continue;
        const KbmRaw& k = bySlot[slot];
        if (!(k.opts & 0x80)) continue;
        float& aim = *reinterpret_cast<float*>(kAimSensitivity + i * 4);
        float want = k.aim / 100.0f;
        if (aim != want) aim = want;
        int view = *reinterpret_cast<int*>(p + 0xdfc);
        if (view >= 0 && view < 4) {
            BYTE& inv = *reinterpret_cast<BYTE*>(kInvertAt + view * kViewSize);
            BYTE wantInv = (k.opts & 1) ? 1 : 0;
            if (inv != wantInv) inv = wantInv;
        }
        BYTE& ret = *reinterpret_cast<BYTE*>(kReticlesAt + i);
        BYTE wantRet = (k.opts & 2) ? 1 : 0;
        if (ret != wantRet) {
            ret = wantRet;
            if (!reinterpret_cast<Bool_t>(0x617260)())
                reinterpret_cast<ReticleApply_t>(0x98f0a0)(p + 0x117b8, wantRet);
        }
    }
}

// The session game's profile (the host's save) has loaded: my own sound and subtitles over it.
void OnlineSettingsProfileLoaded() {
    if (!g_mine.known || !NetplayEnabled()) return;
    for (int i = 0; i < 4; ++i) AudioSetGameVolume(i, g_mine.vol[i]);
    *reinterpret_cast<int*>(kSubtitles) = g_mine.subtitles ? 1 : 0;
    Log(L"Settings: my volumes and subtitles applied");
}

// ---- the lobby game's side ----------------------------------------------------------------------------------
// FGUYMOD_MySettings from the ONLINE screens (the game's own getters): remembered for the next match.
void OnlineSettingsReport(const char* s) {
    Mine m;
    if (Parse(s, &m)) {
        g_mine = m;
        static int logged = 0;
        if (logged++ < 3) Log(L"Settings: my settings %S", s);
    }
}

// Starting a match: mine, for the session game (FGUYMOD_MY_SETTINGS).
std::string OnlineSettingsCapture() { return g_mine.known ? Format(g_mine) : std::string(); }

// Back from a match: what the session game left us ("" if nothing). Read once.
const char* OnlineSettingsBack() {
    wchar_t path[MAX_PATH];
    BackFile(GetCurrentProcessId(), path);
    g_back.clear();
    HANDLE f = CreateFileW(path, GENERIC_READ, 0, nullptr, OPEN_EXISTING, 0, nullptr);
    if (f != INVALID_HANDLE_VALUE) {
        char buf[256] = {};
        DWORD got = 0;
        ReadFile(f, buf, sizeof(buf) - 1, &got, nullptr);
        CloseHandle(f);
        DeleteFileW(path);
        Mine m;
        if (Parse(buf, &m)) {
            g_mine = m;
            g_back = buf;
            Log(L"Settings: back from the match with %S", buf);
        }
    }
    g_vibration = SettingInt(L"Online", L"Vibration", nullptr, 1) != 0;
    return g_back.c_str();
}

// The session game, handing back (handoff.cpp): my settings as they are now, for the lobby game.
void OnlineSettingsWriteBack(DWORD lobbyPid) {
    if (!g_mine.known) return;
    for (int i = 0; i < 4; ++i) g_mine.vol[i] = AudioGameVolume(i);   // (changed in the match, or not)
    wchar_t path[MAX_PATH];
    BackFile(lobbyPid, path);
    std::string s = Format(g_mine);
    HANDLE f = CreateFileW(path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
    DWORD put = 0;
    if (f != INVALID_HANDLE_VALUE) {
        WriteFile(f, s.data(), (DWORD)s.size(), &put, nullptr);
        CloseHandle(f);
    }
}

// ---- the SETTINGS page --------------------------------------------------------------------------------------
enum Row {
    kAudio, kMusic, kEffects, kDialog, kCinematic, kMovies, kSubs,
    kControls, kAim, kInvert, kVibration, kReticles, kInputInfo, kKeysInfo,
    kScreen, kDisplay, kResolution, kFrameRate, kShowPing,
    kBack, kRowCount
};

int OnlineSettingsRows(SettingsRow* out, int max) {
    char v[64];
    auto put = [&](int i, const char* label, const std::string& value, char kind) {
        if (i < max) out[i] = {label, value, kind};
    };
    auto pct = [&](float f) { sprintf_s(v, "%d", Percent(f)); return std::string(v); };   // (the HUD adds the %)
    auto onoff = [](bool b) { return std::string(b ? "ON" : "OFF"); };
    put(kAudio, "AUDIO", "", 'h');
    put(kMusic, "Music", pct(AudioGameVolume(0)), 's');
    put(kEffects, "Sound effects", pct(AudioGameVolume(1)), 's');
    put(kDialog, "Dialog", pct(AudioGameVolume(2)), 's');
    put(kCinematic, "Cinematics", pct(AudioGameVolume(3)), 's');
    put(kMovies, "Movies & title", pct(AudioMovieVolume()), 's');
    put(kSubs, "Subtitles", onoff(*reinterpret_cast<int*>(kSubtitles) != 0), 't');
    put(kControls, "CONTROLS", "", 'h');
    sprintf_s(v, "%d", g_mine.aim);
    put(kAim, "Aim sensitivity", g_mine.known ? std::string(v) : std::string("-"), g_mine.known ? 's' : 'i');
    put(kInvert, "Invert look", g_mine.known ? onoff(g_mine.invert) : std::string("-"), g_mine.known ? 't' : 'i');
    put(kVibration, "Vibration", onoff(g_vibration), 't');
    put(kReticles, "Reticles", g_mine.known ? onoff(g_mine.reticles) : std::string("-"), g_mine.known ? 't' : 'i');
    put(kInputInfo, "Controls", KbmIsKeyboardSlot(NetplayLocalSlot()) ? "Keyboard & mouse (set in the lobby)"
                                                                      : "Controller (set in the lobby)", 'i');
    put(kKeysInfo, "Key bindings", "Main menu > Options > Controls", 'i');
    put(kScreen, "SCREEN", "", 'h');
    int mode = DisplayMenuIndex();   // 0 windowed, 1 fullscreen, 2 borderless
    put(kDisplay, "Display mode", mode == 0 ? "WINDOWED" : mode == 2 ? "BORDERLESS" : "FULLSCREEN", 'c');
    int w = 0, h = 0;
    DisplayRenderSize(&w, &h);
    sprintf_s(v, "%d x %d (main menu > Options)", w, h);
    // (read only: mid-level the game can't reset its graphics device for a new size - it dies)
    put(kResolution, "Resolution", v, 'i');
    sprintf_s(v, "%d fps (the match's)", NetplayRate());
    put(kFrameRate, "Frame rate", v, 'i');
    put(kShowPing, "Show ping", onoff(g_showPing), 't');
    put(kBack, "BACK", "", 'b');
    return kRowCount;
}

void OnlineSettingsChange(int row, int delta) {
    auto step = [&](float now) {
        int p = Percent(now) + (delta < 0 ? -5 : 5);
        p = p < 0 ? 0 : (p > 100 ? 100 : p);
        return p / 100.0f;
    };
    if (row >= kMusic && row <= kCinematic) {
        if (delta) AudioSetGameVolume(row - kMusic, step(AudioGameVolume(row - kMusic)));
    } else if (row == kMovies) {
        if (delta) AudioSetMovieVolume(step(AudioMovieVolume()));
    } else if (row == kSubs) {
        int& s = *reinterpret_cast<int*>(kSubtitles);
        s = s ? 0 : 1;
        g_mine.subtitles = s != 0;
    } else if (row == kAim && g_mine.known) {
        if (delta) g_mine.aim = Percent(step(g_mine.aim / 100.0f));
    } else if (row == kInvert && g_mine.known) {
        g_mine.invert = !g_mine.invert;
    } else if (row == kReticles && g_mine.known) {
        g_mine.reticles = !g_mine.reticles;
    } else if (row == kVibration) {
        g_vibration = !g_vibration;
        SaveVibration();
    } else if (row == kShowPing) {
        g_showPing = !g_showPing;
        SaveFlag(L"ShowPing", g_showPing);
    } else if (row == kDisplay) {
        int mode = DisplayMenuIndex();   // windowed <-> borderless (an online match never runs fullscreen)
        DisplayMenuSelect(mode == 0 ? 2 : 0);
        DisplayMenuApply();
    }
}
