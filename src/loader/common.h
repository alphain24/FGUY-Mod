#pragma once
// Shared bits for the loader's modules.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

extern wchar_t g_gameDir[MAX_PATH];  // folder containing FGUY.exe
extern wchar_t g_modDir[MAX_PATH];   // <game>\FGUYMod

void Log(const wchar_t* fmt, ...);
void OpenLog();

bool FileExists(const wchar_t* path);

// Loader settings live in FGUYMod\FGUYMod.ini; an environment variable of the same meaning wins
// over the file (used by test scripts). Returns the value from env, then file, then the default.
int SettingInt(const wchar_t* section, const wchar_t* key, const wchar_t* envVar, int def);
void SettingString(const wchar_t* section, const wchar_t* key, const wchar_t* envVar,
                   const wchar_t* def, wchar_t* out, DWORD outLen);

// One import-table hook. `fn` is a name or an ordinal (MAKEINTRESOURCEA).
struct HookSpec {
    const char* dll;
    const char* fn;
    void* hook;
    void** real;
};
void InstallHooks(HMODULE exe, HookSpec* hooks, size_t count);
template <size_t N>
void InstallHooks(HMODULE exe, HookSpec (&hooks)[N]) { InstallHooks(exe, hooks, N); }

// Modules
void InitConfigOverride(HMODULE exe);  // fguy.ini override + file-open log   (config.cpp)
const wchar_t* ConfigSaveDir();        //   folder the game's saves are in right now
void InitNetSpy(HMODULE exe);          // winsock + debug-print logging        (netspy.cpp)
void InitDisplay(HMODULE exe);         // windowed / borderless / resolution   (display.cpp)
void InitVirtualPad(HMODULE exe);      // injected controller input for tests  (vpad.cpp)
WORD VPadButtons(DWORD pad);           //   buttons the test pipe holds on a pad right now
void VPadPressLater(DWORD pad, WORD buttons, DWORD ms);
void InitDirectInput();                // Windows key in windowed modes        (dinput.cpp)
void OnDirectInputCreated(void* directInput);

void InitD3D9(HMODULE exe);            // resolutions, frame rate, crash fix   (d3d9.cpp)
int VideoFrameRateIndex();             //   Options > Screen > Frame Rate support
void VideoSetFrameRate(int index);
void VideoRequestQuit();               //   exit cleanly at the end of the next frame (test tools)
void InitUi();                         // menu movie patches + menu calls      (ui.cpp)
void InitResearch();                   // optional research hooks              (research.cpp)
void ResearchOnFrame(unsigned frame);
int ResearchPlayers(void** out, int max);  // live player objects (needs the player spy)

void InitNetplay(HMODULE exe);         // lockstep online play core            (netplay.cpp)
bool NetplayEnabled();
void NetplayOnFrame();                 //   end of every rendered frame
bool NetplayPadConnected(DWORD pad);
int NetplayLocalSlot();                //   my player slot, -1 when netplay is off
void NetplaySessionPress(int pad, WORD buttons, int delayTicks);  // session autopilot
void NetplaySessionLevel();            //   the menu launched the level
void InitSplitView();                  // one full-screen view per PC online    (splitview.cpp)
int SplitViewHudMode(int mode);
int SplitViewHudViewport(int viewport);
void SplitViewMenuRect(float left, float top, float right, float bottom);  // lobby: 3D scene inside a panel

// online.cpp: lobby + the in-game ONLINE screens
void OnlineEnter();
void OnlineExit();
const char* LobbyState();
void LobbyDo(const char* command, const char* a, const char* b, const char* c, const char* d, const char* e);
const char* SessionConfig();

// display.cpp
bool DisplayIsWindowed();              // windowed or borderless right now
int DisplayMenuIndex();                // Options > Screen > Display Mode support
bool DisplayMenuSelect(int index);     //   returns the game's own "windowed" flag for that choice
void DisplayMenuApply();
void DisplayMenuReset();
void DisplayOnChangeResolution(int width, int height, bool windowed);
void DisplayReveal();                  // show the hidden online-session window
void DisplayHideWindow();              // hide the window (old game handing over to the session)

// handoff.cpp: the old game shows the loading screen while the session boots hidden
void InitHandoff();
bool HandoffHidden();                  //   this game is a session booting hidden
void HandoffMatchRunning();            //   the match is running: show it
void HandoffRevealed();
