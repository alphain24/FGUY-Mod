// Seamless start of an online session.
//
// When the host starts a match, every player's game restarts into the session (online.cpp). So that
// the player doesn't watch the game close, reopen and click through its menus, the old game stays on
// screen with the lobby's loading screen while the new one boots hidden and muted: its window is created
// invisible (display.cpp), and it runs title, save and menus by itself (the session autopilot). Once
// the level is running (its HUD lays itself out, ui.cpp HudSplitMode) the new game shows its window, takes the
// foreground and turns its sound on, then sets a named event; the old game sees it and quits.
//
// The old game passes its process id in FGUYMOD_HANDOFF. Event: Local\FGUYMod_SessionReady_<new pid>.
// If the match never starts, the new game shows itself anyway after kRevealTimeoutMs.
//
// The old game doesn't quit, though: it waits hidden (online.cpp OnlinePark). It's the player's own game -
// their save, settings and controllers - so after the match the session game hands back to it
// (HandoffGiveBack): Local\FGUYMod_LobbyReturn_<old pid> tells it to come back (where the session game's window
// is), and once it shows it sets Local\FGUYMod_LobbyBack_<old pid>; the session game then closes.

#include "common.h"

#include <mmdeviceapi.h>
#include <audiopolicy.h>

#include <cwchar>

#pragma comment(lib, "ole32.lib")

namespace {

const DWORD kRevealTimeoutMs = 180000;  // from boot
const DWORD kRevealDelayMs = 300;       // after the level is running (into its fade-in)

bool g_on = false;
DWORD g_oldPid = 0;             // the game we took over from (it waits hidden for the match to end)
volatile LONG g_giveBack = 0;   // HandoffGiveBack: 1 handing back, 2 handed back, 3 it didn't come
HANDLE g_readyEvent = nullptr;
HANDLE g_unmute = nullptr;      // set when the window is shown
volatile LONG g_revealAsked = 0;
volatile LONG g_revealed = 0;

// Mutes this process's audio session (WASAPI, default output device) until g_unmute, then unmutes.
// Its own thread so COM stays in one place; the game's sound streams join the session's mute later.
// Windows remembers a program's mute in its volume mixer, so a game that ends while muted would leave the
// next one silent: every normal start unmutes it first (arg = 1: just unmute).
DWORD WINAPI AudioThread(LPVOID arg) {
    bool unmuteOnly = arg != nullptr;
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    ISimpleAudioVolume* volume = nullptr;
    IMMDeviceEnumerator* devices = nullptr;
    IMMDevice* device = nullptr;
    IAudioSessionManager* sessions = nullptr;
    if (SUCCEEDED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, __uuidof(IMMDeviceEnumerator),
                                   reinterpret_cast<void**>(&devices))) &&
        SUCCEEDED(devices->GetDefaultAudioEndpoint(eRender, eConsole, &device)) &&
        SUCCEEDED(device->Activate(__uuidof(IAudioSessionManager), CLSCTX_ALL, nullptr,
                                   reinterpret_cast<void**>(&sessions))) &&
        SUCCEEDED(sessions->GetSimpleAudioVolume(nullptr, FALSE, &volume))) {
        if (unmuteOnly) {
            BOOL muted = FALSE;
            if (SUCCEEDED(volume->GetMute(&muted)) && muted) {
                volume->SetMute(FALSE, nullptr);
                Log(L"Handoff: the game's sound was left muted in Windows - unmuted");
            }
        } else {
            volume->SetMute(TRUE, nullptr);
            Log(L"Handoff: sound muted until the match shows");
        }
    } else {
        Log(L"Handoff: couldn't reach the game's sound session");
    }
    DWORD r = unmuteOnly ? WAIT_OBJECT_0 : WaitForSingleObject(g_unmute, kRevealTimeoutMs);
    if (r == WAIT_TIMEOUT) {
        Log(L"Handoff: the match didn't start in time - showing the game anyway");
        DisplayReveal();
        WaitForSingleObject(g_unmute, 10000);
    }
    if (volume) {
        volume->SetMute(FALSE, nullptr);
        volume->Release();
    }
    if (sessions) sessions->Release();
    if (device) device->Release();
    if (devices) devices->Release();
    CoUninitialize();
    return 0;
}

// Mutes / unmutes this process's sound in Windows (its own thread: COM).
DWORD WINAPI MuteThread(LPVOID arg) {
    bool mute = arg != nullptr;
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    ISimpleAudioVolume* volume = nullptr;
    IMMDeviceEnumerator* devices = nullptr;
    IMMDevice* device = nullptr;
    IAudioSessionManager* sessions = nullptr;
    if (SUCCEEDED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, __uuidof(IMMDeviceEnumerator),
                                   reinterpret_cast<void**>(&devices))) &&
        SUCCEEDED(devices->GetDefaultAudioEndpoint(eRender, eConsole, &device)) &&
        SUCCEEDED(device->Activate(__uuidof(IAudioSessionManager), CLSCTX_ALL, nullptr,
                                   reinterpret_cast<void**>(&sessions))) &&
        SUCCEEDED(sessions->GetSimpleAudioVolume(nullptr, FALSE, &volume)))
        volume->SetMute(mute ? TRUE : FALSE, nullptr);
    if (volume) volume->Release();
    if (sessions) sessions->Release();
    if (device) device->Release();
    if (devices) devices->Release();
    CoUninitialize();
    return 0;
}

DWORD WINAPI GiveBackWait(LPVOID arg) {
    HANDLE back = arg;
    if (WaitForSingleObject(back, 15000) == WAIT_OBJECT_0) {
        InterlockedExchange(&g_giveBack, 2);
        Log(L"Handoff: the lobby game is showing - closing this one");
        DisplayHideWindow();
        VideoRequestQuit();
        Sleep(8000);   // (no frame to quit at: the last resort)
        Log(L"Handoff: no frame since the quit - exiting");
        QuitGame();
    } else {
        InterlockedExchange(&g_giveBack, 3);
        Log(L"Handoff: the lobby game didn't come back - the lobby opens here instead");
    }
    CloseHandle(back);
    return 0;
}

DWORD WINAPI RevealLater(LPVOID) {
    Sleep(kRevealDelayMs);
    DisplayReveal();
    return 0;
}

}  // namespace

void InitHandoff() {
    wchar_t old[16];
    if (GetEnvironmentVariableW(L"FGUYMOD_HANDOFF", old, 16)) g_oldPid = wcstoul(old, nullptr, 10);
    if (!g_oldPid) {
        // a normal start: make sure Windows isn't keeping the game muted (the bot / test copies are silent
        // through the game's own volumes instead, audio.cpp - Windows doesn't remember those)
        if (!StaysMuted()) CloseHandle(CreateThread(nullptr, 0, AudioThread, (LPVOID)1, 0, nullptr));
        return;
    }
    if (StaysMuted()) {   // the bot's / a test copy's session: silent through the game's volumes
        SetEnvironmentVariableW(L"FGUYMOD_HANDOFF", nullptr);
        g_on = true;
        wchar_t name[64];
        swprintf_s(name, L"Local\\FGUYMod_SessionReady_%lu", GetCurrentProcessId());
        g_readyEvent = CreateEventW(nullptr, TRUE, FALSE, name);
        g_unmute = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        Log(L"Handoff: booting hidden (game %s is showing the loading screen), no sound", old);
        return;
    }
    SetEnvironmentVariableW(L"FGUYMOD_HANDOFF", nullptr);
    wchar_t show[8];
    if (GetEnvironmentVariableW(L"FGUYMOD_TEST_SHOWSESSION", show, 8)) return;  // test knob: boot visibly
    g_on = true;
    wchar_t name[64];
    swprintf_s(name, L"Local\\FGUYMod_SessionReady_%lu", GetCurrentProcessId());
    g_readyEvent = CreateEventW(nullptr, TRUE, FALSE, name);
    g_unmute = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    CloseHandle(CreateThread(nullptr, 0, AudioThread, nullptr, 0, nullptr));
    Log(L"Handoff: booting hidden (game %s is showing the loading screen)", old);
}

bool HandoffHidden() { return g_on && !g_revealed; }

volatile LONG g_hudUp = 0;
// Booting hidden, before the match runs: nobody watches, so it needn't run at real speed (d3d9.cpp).
bool HandoffBooting() { return g_on && !g_hudUp; }

bool HandoffHudUp() { return g_hudUp != 0; }

// The level is running (its HUD has laid itself out). Show it a moment later.
void HandoffMatchRunning() {
    InterlockedExchange(&g_hudUp, 1);
    if (!g_on || InterlockedExchange(&g_revealAsked, 1)) return;
    Log(L"Handoff: the match is running");
    CloseHandle(CreateThread(nullptr, 0, RevealLater, nullptr, 0, nullptr));
}

// display.cpp, on the window's thread, right after showing it.
void HandoffRevealed() {
    if (!g_on || InterlockedExchange(&g_revealed, 1)) return;
    SetEvent(g_unmute);
    if (g_readyEvent) SetEvent(g_readyEvent);
    Log(L"Handoff: window shown - the old game can close");
}

// The lobby game (old, waiting hidden): its sound off / on in Windows while it waits (the bot and test copies
// are silent anyway, through the game's volumes).
void HandoffMute(bool mute) {
    if (StaysMuted()) return;
    CloseHandle(CreateThread(nullptr, 0, MuteThread, mute ? (LPVOID)1 : nullptr, 0, nullptr));
}

// The match is over (netplay.cpp): hand back to the player's own game, waiting hidden since the match started.
// False if there's none (this game then opens the lobby itself).
bool HandoffGiveBack() {
    if (!g_oldPid || g_giveBack) return g_giveBack == 1 || g_giveBack == 2;
    wchar_t name[64];
    swprintf_s(name, L"Local\\FGUYMod_LobbyReturn_%lu", g_oldPid);
    HANDLE ret = OpenEventW(EVENT_MODIFY_STATE, FALSE, name);
    swprintf_s(name, L"Local\\FGUYMod_LobbyBack_%lu", g_oldPid);
    HANDLE back = OpenEventW(SYNCHRONIZE, FALSE, name);
    if (!ret || !back) {
        if (ret) CloseHandle(ret);
        if (back) CloseHandle(back);
        Log(L"Handoff: the lobby game (process %lu) isn't there any more", g_oldPid);
        g_giveBack = 3;
        return false;
    }
    g_giveBack = 1;
    OnlineSettingsWriteBack(g_oldPid);    // my settings as they are now (changed in the match or not)
    AllowSetForegroundWindow(g_oldPid);   // so it can come to the front
    SetEvent(ret);
    CloseHandle(ret);
    Log(L"Handoff: the match is over - handing back to the lobby game (process %lu)", g_oldPid);
    CloseHandle(CreateThread(nullptr, 0, GiveBackWait, back, 0, nullptr));
    return true;
}

// For the menus: 0 no hand-back (this game opens the lobby), 1 handing back, 2 handed back (this game closes).
int HandoffGiveBackState() {
    LONG s = g_giveBack;
    return s == 1 || s == 2 ? (int)s : 0;
}
