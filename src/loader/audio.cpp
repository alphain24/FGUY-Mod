// Volume: quieter low end for the game's own sliders, movie volume, and the volume our own sounds play at.
//
// The game's sound manager (a static object at 0xb59ea8) keeps the Options > Audio settings as 0..1 floats:
// +0x3ac music, +0x3b0 sound effects, +0x3b4 cinematics, +0x3b8 dialog. One function (0x665d50) turns them
// into the FMOD category volumes after any change. The sliders move in steps of 0.1 (the menu patch makes
// that 0.05) and the game uses them as-is, so 10% is still fairly loud. We hand that function the squares
// of the settings instead (10% -> 1%, 50% -> 25%: the usual "sounds even" curve) and put the real values
// back right after, so the menu and the save keep showing what was picked.
//
// Movies & title: the mod's own Options > Audio row, MOVIES & TITLE (the menu patch; SNDS_GetMovieVolume /
// FGUYMOD_SetMovieVolume, kept in FGUYMod\audio.ini). Movies (Bink) play at it instead of the cinematic
// setting (the game sets their volume when it opens one, 0x4ea2d0 -> BinkSetVolume; they're mixed far
// louder than the game). And until the profile's settings are in - the intro, the "press anywhere to start"
// screen and the save slots, while the game's own settings are still at their 100% defaults - every sound
// plays at it too. (The game reads the save file on the save-slot screen already, but applies the profile's
// settings only once you pick a slot: the main menu opening is the sign - the menu patch tells us.)

#include "common.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace {

const uintptr_t kSoundManager = 0xb59ea8;
const int kMusic = 0x3ac, kEffects = 0x3b0, kCinematic = 0x3b4, kDialog = 0x3b8;
const uintptr_t kApplyVolumes = 0x665d50;  // void __thiscall Apply(SoundManager*)
// push ecx; movss xmm0, [0x9f1fc4]  (9 bytes, moved to the trampoline as-is)
const unsigned char kApplyStart[] = {0x51, 0xf3, 0x0f, 0x10, 0x05, 0xc4, 0x1f, 0x9f, 0x00};
const float kGameScale = 0.6f;             // the game's own factor on its categories (0x9f3c90)
const float kMovieScale = 0.6f;            // movies are mixed far louder than the game (with the curve)
const float kMovieDefault = 0.5f;          // MOVIES & TITLE before it's ever been set

wchar_t g_iniPath[MAX_PATH];
float g_savedMusic = 1.0f;
float g_movie = kMovieDefault;              // MOVIES & TITLE, 0..1
bool g_settingsLoaded = false;              // the save's settings have been applied once
bool g_testRun = false;                     // test copies don't write audio.ini
using Apply_t = void(__fastcall*)(void* self, void* edx);
Apply_t g_applyOriginal = nullptr;

float& Setting(int off) { return *reinterpret_cast<float*>(kSoundManager + off); }

float Curve(float v) {
    if (!(v > 0)) return 0;
    if (v > 1) v = 1;
    return v * v;
}

void Remember(const wchar_t* key, float v, float& saved) {
    if (g_testRun || v == saved) return;
    saved = v;
    wchar_t text[32];
    swprintf_s(text, L"%.2f", v);
    WritePrivateProfileStringW(L"Audio", key, text, g_iniPath);
}

void __fastcall Hook_ApplyVolumes(void* self, void* edx) {
    const int offs[4] = {kMusic, kEffects, kCinematic, kDialog};
    float real[4];
    for (int i = 0; i < 4; ++i) real[i] = Setting(offs[i]);
    static int logged = 0;
    if (logged < 6) {
        ++logged;
        Log(L"Audio: volumes music %.2f, effects %.2f, cinematic %.2f, dialog %.2f", real[0], real[1], real[2], real[3]);
    }
    if (g_settingsLoaded) Remember(L"Music", real[0], g_savedMusic);
    for (int i = 0; i < 4; ++i)   // bot / test copies: silent; before the save: everything at MOVIES & TITLE
        Setting(offs[i]) = StaysMuted() ? 0.0f : Curve(g_settingsLoaded ? real[i] : g_movie);
    g_applyOriginal(self, edx);
    for (int i = 0; i < 4; ++i) Setting(offs[i]) = real[i];
}

using BinkSetVolume_t = void(__stdcall*)(void* bink, unsigned track, int volume);
BinkSetVolume_t Real_BinkSetVolume = nullptr;

void __stdcall Hook_BinkSetVolume(void* bink, unsigned track, int volume) {
    // The game passed cinematic * 32768 * (something); keep its "something", use MOVIES & TITLE instead.
    float now = Setting(kCinematic);
    double base = now > 0.001f ? volume / now : volume;
    int out = StaysMuted() ? 0 : (int)(base * Curve(g_movie) * kMovieScale);
    if (out < 0) out = 0;
    static int logged = 0;
    if (logged < 8) {
        ++logged;
        Log(L"Audio: movie volume %d -> %d (movies & title %.2f)", volume, out, g_movie);
    }
    Real_BinkSetVolume(bink, track, out);
}

bool MakeTrampoline() {
    auto at = reinterpret_cast<unsigned char*>(kApplyVolumes);
    if (IsBadReadPtr(at, sizeof(kApplyStart)) || memcmp(at, kApplyStart, sizeof(kApplyStart)) != 0) {
        Log(L"Audio: volume code isn't what we expect (different game version?) - volume curve off");
        return false;
    }
    auto t = static_cast<unsigned char*>(VirtualAlloc(nullptr, 32, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
    if (!t) return false;
    memcpy(t, kApplyStart, sizeof(kApplyStart));
    t[sizeof(kApplyStart)] = 0xE9;
    *reinterpret_cast<int*>(t + sizeof(kApplyStart) + 1) =
        (int)((kApplyVolumes + sizeof(kApplyStart)) - (reinterpret_cast<uintptr_t>(t) + sizeof(kApplyStart) + 5));
    g_applyOriginal = reinterpret_cast<Apply_t>(t);
    DWORD old;
    VirtualProtect(at, 5, PAGE_EXECUTE_READWRITE, &old);
    at[0] = 0xE9;
    *reinterpret_cast<int*>(at + 1) = (int)(reinterpret_cast<uintptr_t>(&Hook_ApplyVolumes) - (kApplyVolumes + 5));
    VirtualProtect(at, 5, old, &old);
    FlushInstructionCache(GetCurrentProcess(), at, 5);
    return true;
}

float ReadSaved(const wchar_t* key, const wchar_t* def = L"1") {
    wchar_t text[32];
    GetPrivateProfileStringW(L"Audio", key, def, text, 32, g_iniPath);
    float v = (float)_wtof(text);
    return v < 0 ? 0 : (v > 1 ? 1 : v);
}

}  // namespace

void InitAudio(HMODULE exe) {
    swprintf_s(g_iniPath, L"%s\\audio.ini", g_modDir);
    g_savedMusic = ReadSaved(L"Music");
    g_movie = ReadSaved(L"MoviesAndTitle", L"0.5");
    wchar_t v[8];
    g_testRun = GetEnvironmentVariableW(L"FGUYMOD_SAVEDIR", v, 8) != 0;
    bool curve = MakeTrampoline();
    HookSpec hooks[] = {
        {"binkw32.dll", "_BinkSetVolume@12", (void*)Hook_BinkSetVolume, (void**)&Real_BinkSetVolume},
    };
    InstallHooks(exe, hooks);
    Log(L"Audio: volume curve %s; movies & title %.2f", curve ? L"on" : L"off", g_movie);
}

// Options > Audio > MOVIES & TITLE (the menu patch): 0..1.
float AudioMovieVolume() { return g_movie; }
void AudioSetMovieVolume(float v) {
    if (!(v >= 0)) v = 0;
    if (v > 1) v = 1;
    v = (float)((int)(v * 100 + 0.5f)) / 100;   // the slider's own float steps drift (0.35 -> 0.3499...)
    if (v == g_movie) return;
    g_movie = v;
    wchar_t text[32];
    swprintf_s(text, L"%.2f", v);
    WritePrivateProfileStringW(L"Audio", L"MoviesAndTitle", text, g_iniPath);
}

// The main menu opened (FGUYMOD_ProfileLoaded from the menu patch): the profile's settings are in.
void AudioProfileLoaded() {
    if (g_settingsLoaded) return;
    g_settingsLoaded = true;
    Log(L"Audio: the profile is loaded - volumes from the game's settings now");
    if (g_applyOriginal) Hook_ApplyVolumes(reinterpret_cast<void*>(kSoundManager), nullptr);   // apply them now
}

// The volume our own sounds (sounds.cpp) should play at, 0..1: the music setting the same way the game
// plays its music (curve, 0.6), or the remembered one before the save has loaded.
float AudioMusicVolume() {
    float music = g_settingsLoaded ? Setting(kMusic) : g_savedMusic;
    return Curve(music) * kGameScale;
}
