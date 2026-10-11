// Volume: quieter low end for the game's own sliders, movie volume, and the volume our own sounds play at.
//
// The game's sound manager (a static object at 0xb59ea8) keeps the Options > Audio settings as 0..1 floats:
// +0x3ac music, +0x3b0 sound effects, +0x3b4 cinematics, +0x3b8 dialog. One function (0x665d50) turns them
// into the FMOD category volumes after any change. The sliders move in steps of 0.1 (the menu patch makes
// that 0.05) and the game uses them as-is, so 10% is still fairly loud. We hand that function the squares
// of the settings instead (10% -> 1%, 50% -> 25%: the usual "sounds even" curve) and put the real values
// back right after, so the menu and the save keep showing what was picked.
//
// Movies (Bink): the game sets their volume from the cinematic setting when it opens one (0x4ea2d0 ->
// BinkSetVolume). They play at full scale, louder than the rest of the game (which runs at 0.6), and the
// intro logos play before the save (and its settings) has even loaded. So movies get the same curve, the
// game's 0.6, and until the save is loaded the cinematic setting remembered from last time
// (FGUYMod\audio.ini).

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
const float kMovieScale = 0.15f;           // movies are mixed far louder than the game

wchar_t g_iniPath[MAX_PATH];
float g_savedCinematic = 1.0f, g_savedMusic = 1.0f;
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
    if (g_settingsLoaded) {
        Remember(L"Music", real[0], g_savedMusic);
        Remember(L"Cinematic", real[2], g_savedCinematic);
    }
    for (int i = 0; i < 4; ++i) Setting(offs[i]) = StaysMuted() ? 0.0f : Curve(real[i]);  // bot / test copies: silent
    g_applyOriginal(self, edx);
    for (int i = 0; i < 4; ++i) Setting(offs[i]) = real[i];
}

using BinkSetVolume_t = void(__stdcall*)(void* bink, unsigned track, int volume);
BinkSetVolume_t Real_BinkSetVolume = nullptr;

void __stdcall Hook_BinkSetVolume(void* bink, unsigned track, int volume) {
    // The game passed cinematic * 32768 * (something); keep its "something", replace the setting part.
    float now = Setting(kCinematic);
    float setting = g_settingsLoaded ? now : g_savedCinematic;
    double base = now > 0.001f ? volume / now : volume;
    int out = StaysMuted() ? 0 : (int)(base * Curve(setting) * kMovieScale);
    if (out < 0) out = 0;
    static int logged = 0;
    if (logged < 8) {
        ++logged;
        Log(L"Audio: movie volume %d -> %d (cinematic setting %.2f%s)", volume, out, setting,
            g_settingsLoaded ? L"" : L", remembered from last time");
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

float ReadSaved(const wchar_t* key) {
    wchar_t text[32];
    GetPrivateProfileStringW(L"Audio", key, L"1", text, 32, g_iniPath);
    float v = (float)_wtof(text);
    return v < 0 ? 0 : (v > 1 ? 1 : v);
}

}  // namespace

void InitAudio(HMODULE exe) {
    swprintf_s(g_iniPath, L"%s\\audio.ini", g_modDir);
    g_savedCinematic = ReadSaved(L"Cinematic");
    g_savedMusic = ReadSaved(L"Music");
    wchar_t v[8];
    g_testRun = GetEnvironmentVariableW(L"FGUYMOD_SAVEDIR", v, 8) != 0;
    bool curve = MakeTrampoline();
    HookSpec hooks[] = {
        {"binkw32.dll", "_BinkSetVolume@12", (void*)Hook_BinkSetVolume, (void**)&Real_BinkSetVolume},
    };
    InstallHooks(exe, hooks);
    Log(L"Audio: volume curve %s; movies at the cinematic setting (last time %.2f)", curve ? L"on" : L"off",
        g_savedCinematic);
}

// config.cpp: the game has read its save (its settings are applied from here on).
void AudioSaveLoaded() {
    if (!g_settingsLoaded) Log(L"Audio: the save is loaded - volumes from the game's settings now");
    g_settingsLoaded = true;
}

// The volume our own sounds (sounds.cpp) should play at, 0..1: the music setting the same way the game
// plays its music (curve, 0.6), or the remembered one before the save has loaded.
float AudioMusicVolume() {
    float music = g_settingsLoaded ? Setting(kMusic) : g_savedMusic;
    return Curve(music) * kGameScale;
}
