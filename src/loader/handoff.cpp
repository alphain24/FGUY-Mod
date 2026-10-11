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

#include "common.h"

#include <mmdeviceapi.h>
#include <audiopolicy.h>

#include <cwchar>

#pragma comment(lib, "ole32.lib")

namespace {

const DWORD kRevealTimeoutMs = 180000;  // from boot
const DWORD kRevealDelayMs = 300;       // after the level is running (into its fade-in)

bool g_on = false;
HANDLE g_readyEvent = nullptr;
HANDLE g_unmute = nullptr;      // set when the window is shown
volatile LONG g_revealAsked = 0;
volatile LONG g_revealed = 0;

// Mutes this process's audio session (WASAPI, default output device) until g_unmute, then unmutes.
// Its own thread so COM stays in one place; the game's sound streams join the session's mute later.
DWORD WINAPI AudioThread(LPVOID) {
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
        volume->SetMute(TRUE, nullptr);
        Log(StaysMuted() ? L"Handoff: test copy - sound off" : L"Handoff: sound muted until the match shows");
    } else {
        Log(L"Handoff: couldn't mute the sound while hidden");
    }
    if (StaysMuted()) return 0;   // the test bot / test copies stay muted
    DWORD r = WaitForSingleObject(g_unmute, kRevealTimeoutMs);
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

DWORD WINAPI RevealLater(LPVOID) {
    Sleep(kRevealDelayMs);
    DisplayReveal();
    return 0;
}

}  // namespace

void InitHandoff() {
    wchar_t old[16];
    if (!GetEnvironmentVariableW(L"FGUYMOD_HANDOFF", old, 16)) {
        if (StaysMuted()) CloseHandle(CreateThread(nullptr, 0, AudioThread, nullptr, 0, nullptr));  // muted
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
