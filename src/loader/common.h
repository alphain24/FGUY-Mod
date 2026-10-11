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
bool VPadPressedWithin(WORD buttons, DWORD ms);
bool VPadRealPadConnected();           //   a real controller is plugged in  //   a controller button went down just now
void InitDirectInput();                // Windows key in windowed modes        (dinput.cpp)
void OnDirectInputCreated(void* directInput);

void InitD3D9(HMODULE exe);            // resolutions, frame rate, crash fix   (d3d9.cpp)
int VideoFrameRateIndex();             //   Options > Screen > Frame Rate support
void VideoSetFrameRate(int index);
void VideoRequestQuit();               //   exit cleanly at the end of the next frame (test tools)
void QuitGame();                       //   exit now (the game's own exit-time crash is skipped)
void InitUi();                         // menu movie patches + menu calls      (ui.cpp)
void InitResearch();                   // optional research hooks              (research.cpp)
void ResearchOnFrame(unsigned frame);
int ResearchPlayers(void** out, int max);  // player objects the research spy has seen
int GamePlayers(unsigned char** out, int max);  // the game's live players, by player index (netplay.cpp)

void InitNetplay(HMODULE exe);         // lockstep online play core            (netplay.cpp)
bool NetplayEnabled();
void NetplayOnFrame();                 //   end of every rendered frame
bool NetplayPadConnected(DWORD pad);
int NetplayLocalSlot();                //   my player slot, -1 when netplay is off
unsigned NetplayTick();                //   the lockstep tick being simulated
void NetplayLogStacks();               //   research (test pipe "stacks")
bool NetplayLevelLive();               //   session: every player's level HUD is up (same tick everywhere)
void NetplaySessionPress(int pad, WORD buttons, int delayTicks);  // session autopilot
void NetplaySessionLevel();            //   the menu launched the level
void NetplayLeave(const wchar_t* why); //   leave the session (back to the lobby): run on our own
bool NetplaySessionReturn();           //   the menus: the match is over - back to the lobby (leaves the session)
void InitSplitView();                  // one full-screen view per PC online    (splitview.cpp)
int SplitViewHudMode(int mode);
int SplitViewHudViewport(int viewport);
void SplitViewMenuRect(float left, float top, float right, float bottom);  // lobby: 3D scene inside a panel

// onlinemenu.cpp: keyboard / mouse helpers for the ONLINE screens, and the test bot
void OnlineMenuOpen(bool open);
void OnlineMenuKey(HWND wnd, WPARAM vk);    //   display.cpp: a key went down in the game window
const char* OnlineMenuTyped();
bool OnlineMenuFromWASD(int key);
void OnlineMenuTurnPreview(float radians);
void OnlineMenuFrame();                     //   d3d9.cpp, every frame
bool IsBot();                               //   this copy is the test bot (FGUYMOD_BOT)
bool StaysMuted();                          //   the bot / test copies: no sound
const char* BotJoinCode();

// onlinepause.cpp: the online pause menu (the match keeps running; the game pauses once everyone has it open)
struct KbmRaw;
bool OnlinePauseSample(WORD* buttons, SHORT* ly, KbmRaw* kbm, bool keyboardPlayer);  // netplay: my input
int OnlinePauseTick(const BYTE* flags, const bool* gone, int players, bool* keyboard); //   everyone's
void OnlinePauseCommand(const char* command);  //   ui.cpp: a menu command ran
const char* OnlinePauseMenu();                 //   FGUYMOD_PauseMenu (the HUD)
const char* OnlinePauseCursor();
void OnlinePauseQuitDone();

// online.cpp: lobby + the in-game ONLINE screens
void OnlineEnter();
void OnlineRejoin(const char* code, const char* token, int slot);   //   after a match: back to its lobby
void OnlineExit();
const char* LobbyState();
void LobbyDo(const char* command, const char* a, const char* b, const char* c, const char* d, const char* e);
const char* SessionConfig();

// display.cpp
bool DisplayIsWindowed();              // windowed or borderless right now
void DisplayClientToGame(POINT* pt);   // window client pixels -> the game's (borderless render size)
int DisplayMenuIndex();                // Options > Screen > Display Mode support
bool DisplayMenuSelect(int index);     //   returns the game's own "windowed" flag for that choice
void DisplayMenuApply();
void DisplayMenuReset();
bool DisplayOnChangeResolution(int* width, int* height, bool windowed);  // may change the size (borderless)
void DisplayReveal();                  // show the hidden online-session window
void DisplayShowForQuit();             // test tools' quit: bring the window up so it draws its last frame
void DisplayHideWindow();              // hide the window (old game handing over to the session)

// handoff.cpp: the old game shows the loading screen while the session boots hidden
void InitHandoff();
bool HandoffHidden();                  //   this game is a session booting hidden
void HandoffMatchRunning();            //   the match is running: show it
void HandoffRevealed();
bool HandoffHudUp();                   //   this game's level HUD has laid itself out (ui.cpp)
bool HandoffBooting();                 //   booting hidden, the match not running yet

// kbm.cpp: the game's own keyboard / mouse controls in online sessions
#pragma pack(push, 1)
struct KbmRaw {            // one player's raw keyboard and mouse for one tick (sent with their inputs)
    BYTE keys[32];         //   key down bits, by Windows key code
    SHORT dx, dy, dz;      //   mouse movement and wheel
    BYTE buttons;          //   mouse buttons (bit 0 left, 1 right, 2 middle, 3/4 side)
    SHORT cursorX, cursorY; //   mouse cursor in the window, 0..32767 across / down (every PC's window differs)
    BYTE flags;            //   bit 0: this player's level HUD is up (every player's: keyboard mode starts)
};
#pragma pack(pop)
void KbmInit(HWND wnd);
bool KbmOnMessage(HWND wnd, UINT msg, WPARAM wp, LPARAM lp, LRESULT* result);
bool KbmSkipPressed();
void KbmSessionStart(const char* session);     // netplay.cpp: which players play with keyboard and mouse
void KbmLeaveSession();                         //   netplay.cpp: the session is over for this game
bool KbmIsKeyboardSlot(int slot);
bool KbmCursorScreen(POINT* pt);               //   netplay.cpp: the keyboard players' cursor this tick, on screen
void KbmCapture(KbmRaw* out);                  //   my keyboard and mouse now
void KbmApply(const KbmRaw* bySlot, int players);  //   every tick: feed the keyboard players' devices
void KbmTestCommand(const char* what, int a, int b, int c);  // test pipe: kbm key <vk> <ms> | mouse <dx> <dy> <ticks> | click <ms>

// sounds.cpp: loading-screen music
void InitAudio(HMODULE exe);            // volume curve, movie volume            (audio.cpp)
void AudioSaveLoaded();                //   config.cpp: the game read its save
float AudioMusicVolume();              //   0..1, for our own sounds
float AudioMovieVolume();              //   Options > Audio > MOVIES & TITLE, 0..1
void AudioSetMovieVolume(float v);
void InitSounds();
void SoundsOnFileOpen(const wchar_t* path);       //   config.cpp: a file the game opens
void SoundsOnlineLoading(const char* mode, const char* map);  // the lobby's loading screen
