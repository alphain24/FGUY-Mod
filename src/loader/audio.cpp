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
// settings only once you pick a slot: the main menu opening is the sign - the menu patch tells us.) Back
// to the title screen later (the start patch tells us), the same holds until the main menu is back.

#include "common.h"

#include <cmath>
#include <intrin.h>
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
bool g_report = false;                      // FGUYMOD_AUDIOREPORT (AudioReport): test copies keep their volumes
volatile LONG g_silent = 0;                 // AudioSilenceGame: the game's own sound off (its settings stay)
volatile LONG g_silentPending = 0;          //   to apply on the game thread (AudioTick)
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
        Setting(offs[i]) = (StaysMuted() && !g_report) || g_silent ? 0.0f : Curve(g_settingsLoaded ? real[i] : g_movie);
    g_applyOriginal(self, edx);
    for (int i = 0; i < 4; ++i) Setting(offs[i]) = real[i];
}

using BinkSetVolume_t = void(__stdcall*)(void* bink, unsigned track, int volume);
BinkSetVolume_t Real_BinkSetVolume = nullptr;

void __stdcall Hook_BinkSetVolume(void* bink, unsigned track, int volume) {
    // The game passed cinematic * 32768 * (something); keep its "something", use MOVIES & TITLE instead.
    float now = Setting(kCinematic);
    double base = now > 0.001f ? volume / now : volume;
    int out = (StaysMuted() && !g_report) || g_silent ? 0 : (int)(base * Curve(g_movie) * kMovieScale);
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

// Online every PC shows only its own player, but the game still tells FMOD about every viewport's listener
// (split screen: you hear what's near any of the players). There: one listener, mine (its viewport's
// position goes in as listener 0, the others' are dropped). Only the mix changes; the game doesn't read it.
//   FMOD::System::set3DNumListeners(int)                                   (fmodex.dll, __stdcall member)
//   FMOD::System::set3DListenerAttributes(int, pos, vel, forward, up)
using SetNumListeners_t = int(__stdcall*)(void* system, int count);
using SetListener_t = int(__stdcall*)(void* system, int listener, const void* pos, const void* vel,
                                      const void* forward, const void* up);
SetNumListeners_t Real_SetNumListeners = nullptr;
SetListener_t Real_SetListener = nullptr;

int __stdcall Hook_SetNumListeners(void* system, int count) {
    if (SplitViewMyViewport() >= 0 && count > 1) count = 1;
    return Real_SetNumListeners(system, count);
}

// Research (FGUYMOD_AUDIOREPORT=1): in a match, every 5 s, what FMOD is playing - the channels, and each channel
// group (the game's sound categories) with its volume, paused and muted state.
void AudioReport(void* system) {
    static int reports = 0;
    static ULONGLONG next = 0;
    if (!g_report || !NetplayLevelLive() || reports >= 12 || GetTickCount64() < next) return;
    next = GetTickCount64() + 5000;
    ++reports;
    HMODULE fm = GetModuleHandleW(L"fmodex.dll");
    if (!fm) return;
    using GetInt_t = int(__stdcall*)(void* self, int* out);
    using GetPtr_t = int(__stdcall*)(void* self, void** out);
    using GetGroup_t = int(__stdcall*)(void* self, int index, void** out);
    using GetName_t = int(__stdcall*)(void* self, char* name, int len);
    using GetFloat_t = int(__stdcall*)(void* self, float* out);
    using GetBool_t = int(__stdcall*)(void* self, bool* out);
    auto playing = reinterpret_cast<GetInt_t>(GetProcAddress(fm, "?getChannelsPlaying@System@FMOD@@QAG?AW4FMOD_RESULT@@PAH@Z"));
    auto master = reinterpret_cast<GetPtr_t>(GetProcAddress(fm, "?getMasterChannelGroup@System@FMOD@@QAG?AW4FMOD_RESULT@@PAPAVChannelGroup@2@@Z"));
    auto groups = reinterpret_cast<GetInt_t>(GetProcAddress(fm, "?getNumGroups@ChannelGroup@FMOD@@QAG?AW4FMOD_RESULT@@PAH@Z"));
    auto group = reinterpret_cast<GetGroup_t>(GetProcAddress(fm, "?getGroup@ChannelGroup@FMOD@@QAG?AW4FMOD_RESULT@@HPAPAV12@@Z"));
    auto name = reinterpret_cast<GetName_t>(GetProcAddress(fm, "?getName@ChannelGroup@FMOD@@QAG?AW4FMOD_RESULT@@PADH@Z"));
    auto volume = reinterpret_cast<GetFloat_t>(GetProcAddress(fm, "?getVolume@ChannelGroup@FMOD@@QAG?AW4FMOD_RESULT@@PAM@Z"));
    auto paused = reinterpret_cast<GetBool_t>(GetProcAddress(fm, "?getPaused@ChannelGroup@FMOD@@QAG?AW4FMOD_RESULT@@PA_N@Z"));
    auto mute = reinterpret_cast<GetBool_t>(GetProcAddress(fm, "?getMute@ChannelGroup@FMOD@@QAG?AW4FMOD_RESULT@@PA_N@Z"));
    auto channels = reinterpret_cast<GetInt_t>(GetProcAddress(fm, "?getNumChannels@ChannelGroup@FMOD@@QAG?AW4FMOD_RESULT@@PAH@Z"));
    if (!playing || !master || !groups || !group || !name || !volume || !paused || !mute || !channels) return;
    int n = 0;
    playing(system, &n);
    // the mix as it goes out (this copy is muted in Windows, not in the game: see InitAudio)
    using Wave_t = int(__stdcall*)(void* self, float* out, int count, int channel);
    auto wave = reinterpret_cast<Wave_t>(GetProcAddress(fm, "?getWaveData@System@FMOD@@QAG?AW4FMOD_RESULT@@PAMHH@Z"));
    static float samples[2048];
    double sum = 0, peak = 0;
    if (wave && wave(system, samples, 2048, 0) == 0)
        for (float x : samples) { sum += x * x; if (std::fabs(x) > peak) peak = std::fabs(x); }
    wchar_t line[2048];
    int len = swprintf_s(line, L"Audio: tick %u, %d channels playing, output rms %.4f peak %.4f;", NetplayTick(), n,
                         std::sqrt(sum / 2048), peak);
    struct Walk { void* g; int depth; };
    Walk stack[64];
    int top = 0;
    void* m = nullptr;
    if (master(system, &m) == 0 && m) stack[top++] = {m, 0};
    while (top > 0 && len < 1900) {
        Walk w = stack[--top];
        char nm[64] = "";
        float vol = -1;
        bool p = false, mu = false;
        int ch = 0, kids = 0;
        name(w.g, nm, 63);
        volume(w.g, &vol);
        paused(w.g, &p);
        mute(w.g, &mu);
        channels(w.g, &ch);
        groups(w.g, &kids);
        len += swprintf_s(line + len, 2048 - len, L" %*s%S vol %.2f%s%s ch %d;", w.depth * 2, L"", nm[0] ? nm : "(master)", vol,
                          p ? L" PAUSED" : L"", mu ? L" MUTED" : L"", ch);
        for (int i = kids - 1; i >= 0 && top < 64; --i) {
            void* c = nullptr;
            if (group(w.g, i, &c) == 0 && c && w.depth < 3) stack[top++] = {c, w.depth + 1};
        }
    }
    Log(L"%s", line);
}

// Research (with the report): the game pauses / mutes a sound category while any of its reasons holds -
// 0x6367e0 __thiscall (manager, int category, bool on, unsigned reasons): reasons at manager + category * 12 + 0x3c0,
// its name at +0x3c4, then the category's +0x30 (on = any reason). Every change into the log.
extern "C" void __cdecl CategoryLog(const unsigned char* mgr, int category, int on, unsigned bits) {
    static int logged = 0;
    if (!mgr || category < 0 || category > 64 || ++logged > 300) return;
    const unsigned char* e = mgr + category * 12;
    const char* name = *reinterpret_cast<const char* const*>(e + 0x3c4);
    unsigned before = *reinterpret_cast<const unsigned*>(e + 0x3c0);
    unsigned after = (on & 0xff) ? before | bits : before & ~bits;
    wchar_t stack[200] = L"";
    if (on & 0xff) {   // (who: the game addresses up the stack)
        const DWORD* sp = reinterpret_cast<const DWORD*>(_AddressOfReturnAddress());
        int len = 0;
        for (int i = 4, found = 0; i < 600 && found < 10 && len < 180; ++i)
            if (sp[i] >= 0x401000 && sp[i] < 0x9f0000) { len += swprintf_s(stack + len, 200 - len, L" %#lx", sp[i]); ++found; }
    }
    Log(L"Audio: tick %u category %d '%S' reasons %#x %s %#x -> %#x%s", NetplayTick(), category,
        name && !IsBadReadPtr(name, 1) ? name : "?", before, (on & 0xff) ? L"+" : L"-", bits, after, stack);
}
__declspec(naked) void CategoryStub() {
    __asm {
        pushad
        pushfd
        push dword ptr [esp + 48]   // reasons (pushad 32 + pushfd 4 + the return address 4: index at +40)
        push dword ptr [esp + 48]   // on
        push dword ptr [esp + 48]   // category
        push ecx
        call CategoryLog
        add esp, 16
        popfd
        popad
        cmp byte ptr [esp + 8], 0
        push 0x6367e5
        ret
    }
}

int __stdcall Hook_SetListener(void* system, int listener, const void* pos, const void* vel, const void* forward,
                               const void* up) {
    AudioReport(system);
    int mine = SplitViewMyViewport();
    static bool one = false;   // (the count may have been set before the level's views were known)
    if (mine >= 0) {
        if (!one) { one = true; Real_SetNumListeners(system, 1); Log(L"Audio: online - one 3D listener, viewport %d's", mine); }
        if (listener != mine) return 0;   // FMOD_OK: someone else's view
        listener = 0;
    } else {
        one = false;
    }
    return Real_SetListener(system, listener, pos, vel, forward, up);
}

void InitAudio(HMODULE exe) {
    swprintf_s(g_iniPath, L"%s\\audio.ini", g_modDir);
    g_savedMusic = ReadSaved(L"Music");
    g_movie = ReadSaved(L"MoviesAndTitle", L"0.5");
    wchar_t v[8];
    g_testRun = GetEnvironmentVariableW(L"FGUYMOD_SAVEDIR", v, 8) != 0;
    g_report = GetEnvironmentVariableW(L"FGUYMOD_AUDIOREPORT", v, 8) != 0;
    static const unsigned char kCategoryStart[] = {0x80, 0x7c, 0x24, 0x08, 0x00};   // cmp byte ptr [esp+8], 0
    auto cat = reinterpret_cast<unsigned char*>(0x6367e0);
    if (g_report && !memcmp(cat, kCategoryStart, sizeof(kCategoryStart))) {
        DWORD old;
        VirtualProtect(cat, 5, PAGE_EXECUTE_READWRITE, &old);
        cat[0] = 0xE9;
        *reinterpret_cast<int*>(cat + 1) = (int)(reinterpret_cast<uintptr_t>(&CategoryStub) - (0x6367e0 + 5));
        VirtualProtect(cat, 5, old, &old);
        FlushInstructionCache(GetCurrentProcess(), cat, 5);
    }
    if (g_report && StaysMuted())   // (a test copy: its real volumes, but silent in Windows - re-muted now and then)
        CloseHandle(CreateThread(nullptr, 0, [](LPVOID) -> DWORD {
            for (;;) { HandoffMuteInWindows(); Sleep(2000); }
        }, nullptr, 0, nullptr));
    bool curve = MakeTrampoline();
    HookSpec hooks[] = {
        {"binkw32.dll", "_BinkSetVolume@12", (void*)Hook_BinkSetVolume, (void**)&Real_BinkSetVolume},
        {"fmodex.dll", "?set3DNumListeners@System@FMOD@@QAG?AW4FMOD_RESULT@@H@Z", (void*)Hook_SetNumListeners,
         (void**)&Real_SetNumListeners},
        {"fmodex.dll", "?set3DListenerAttributes@System@FMOD@@QAG?AW4FMOD_RESULT@@HPBUFMOD_VECTOR@@000@Z",
         (void*)Hook_SetListener, (void**)&Real_SetListener},
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

// The game's own volume settings, 0..1 (0 music, 1 sound effects, 2 dialog, 3 cinematics) - as Options > Audio.
float AudioGameVolume(int which) {
    const int offs[4] = {kMusic, kEffects, kDialog, kCinematic};
    return which >= 0 && which < 4 ? Setting(offs[which]) : 0;
}
void AudioSetGameVolume(int which, float v) {
    const int offs[4] = {kMusic, kEffects, kDialog, kCinematic};
    if (which < 0 || which >= 4) return;
    Setting(offs[which]) = v < 0 ? 0 : (v > 1 ? 1 : v);
    if (g_applyOriginal) Hook_ApplyVolumes(reinterpret_cast<void*>(kSoundManager), nullptr);
}

// The title screen shows (FGUYMOD_TitleScreen, the start patch): MOVIES & TITLE again until the main menu.
void AudioTitleScreen() {
    if (!g_settingsLoaded) return;
    g_settingsLoaded = false;
    Log(L"Audio: back at the title screen - movies & title volume until the main menu");
    if (g_applyOriginal) Hook_ApplyVolumes(reinterpret_cast<void*>(kSoundManager), nullptr);
}

// Online, starting a match: this (the lobby) game shows the loading screen while the match's game boots hidden -
// its own menu music and sounds would play on underneath (the loading music is ours, sounds.cpp: not affected).
// Off until it comes back from the match (or the start fails). The settings themselves don't change.
void AudioSilenceGame(bool on) {
    if (InterlockedExchange(&g_silent, on ? 1 : 0) == (on ? 1 : 0)) return;
    InterlockedExchange(&g_silentPending, 1);
    Log(L"Audio: the game's own sound %s", on ? L"off (the online loading screen)" : L"on again");
}
// vpad.cpp, the game thread's controller poll (every tick).
void AudioTick() {
    if (g_silentPending && InterlockedExchange(&g_silentPending, 0) && g_applyOriginal)
        Hook_ApplyVolumes(reinterpret_cast<void*>(kSoundManager), nullptr);
}

// The volume our own sounds (sounds.cpp) should play at, 0..1: the music setting the same way the game
// plays its music (curve, 0.6), or the remembered one before the save has loaded.
float AudioMusicVolume() {
    float music = g_settingsLoaded ? Setting(kMusic) : g_savedMusic;
    return Curve(music) * kGameScale;
}
