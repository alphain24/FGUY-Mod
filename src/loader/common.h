#pragma once
// Shared bits for the loader's modules.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <string>
#include <vector>

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
struct _XINPUT_GAMEPAD;
bool VPadHeld(DWORD pad, _XINPUT_GAMEPAD* held);   //   the test pipe's "hold" on a pad, while it lasts
void VPadPressLater(DWORD pad, WORD buttons, DWORD ms);
bool VPadPressedWithin(WORD buttons, DWORD ms);
bool VPadRealPadConnected();           //   a real controller is plugged in  //   a controller button went down just now
void InitDirectInput();                // Windows key in windowed modes        (dinput.cpp)
void OnDirectInputCreated(void* directInput);

void InitD3D9(HMODULE exe);            // resolutions, frame rate, crash fix   (d3d9.cpp)
int VideoFrameRateIndex();             //   Options > Screen > Frame Rate support
void VideoSetFrameRate(int index);
void VideoRequestQuit();               //   exit cleanly at the end of the next frame (test tools)
void VideoRequestShot(const wchar_t* path);   // test tools: save the next frame as a .bmp
void QuitGame();                       //   exit now (the game's own exit-time crash is skipped)
void InitUi();                         // menu movie patches + menu calls      (ui.cpp)
void InitResearch();                   // optional research hooks              (research.cpp)
void ResearchOnFrame(unsigned frame);
int ResearchPlayers(void** out, int max);
void ResearchSpyMark(const char* label);   //   test pipe "spymark": each player's method calls since the last mark  // player objects the research spy has seen
int GamePlayers(unsigned char** out, int max);  // the game's live players, by player index (netplay.cpp)

void InitNetplay(HMODULE exe);         // lockstep online play core            (netplay.cpp)
bool NetplayEnabled();
bool NetplayFreeRun();                 //   the match runs free (no lockstep: client-server, being built)
bool NetplayStoreAllowed(int player);  //   the store (ui.cpp): may this purchase for that player go through here?
void NetplayStoreBought(int player, int weapon);  // my player bought a weapon (sent to the others)
bool NetplayStoreTake(int* player, int* weapon);  // a weapon someone bought on their PC, to mark bought here
void NetplayOnFrame();                 //   end of every rendered frame
bool NetplayPadConnected(DWORD pad);
int NetplayLocalSlot();                //   my player slot, -1 when netplay is off
const wchar_t* NetplayLobbyName(int slot);   // a player's name in the lobby, or ""
void NetplayTestStall(DWORD ms);       //   test pipe: this PC goes quiet (a dropped connection)
void NetplayProfileRender(DWORD ms);   //   research (test pipe "profile <ms>"): the render thread's hot spots
void NetplayTestLeave();               //   test pipe "netleave": quit the match to the lobby (as the pause menu does)
void NetplayTestNudge();
void NetplayTestKillNpcs();                          //   test pipe "killnpcs": the host's enemies all down
void NetplayTestHurt(int player, float amount);   //   test pipe "hurt": my player there takes a hit nobody caused
void OnlineMenuLogYaw();               //   test pipe "yaw": the lobby preview's facing (onlinemenu.cpp)
void NetplayTestResolution(int w, int h);   //   test pipe: the game changes resolution (Options > Screen)               //   test pipe: my player moves a little here only (out of sync)
int NetplaySessionSlot();              //   my player slot in this game's session (also after leaving it)
unsigned NetplayTick();                //   the lockstep tick being simulated
int NetplayRate();                     //   ticks (frames) per second online
void NetplayLogStacks();               //   research (test pipe "stacks")
bool NetplayLevelLive();               //   session: every player's level HUD is up (same tick everywhere)
void NetplaySessionPress(int pad, WORD buttons, int delayTicks);  // session autopilot
void NetplaySessionLevel();            //   the menu launched the level
void NetplayLeave(const wchar_t* why); //   leave the session (back to the lobby): run on our own
bool NetplaySessionReturn();           //   the menus: the match is over - back to the lobby (leaves the session)
void InitLoadSync();
void LoadSyncTickStart();              //   each session tick: wait for menu movies still loading                   // online: level start-up the same tick on every PC (loadsync.cpp)
DWORD NetplayGameThread();             //   the game logic thread (netplay.cpp)
void InitRngTrace();
void CamDump(unsigned tick);           // research: players' cameras every tick (rngtrace.cpp)
void WatchAddress(const void* addr, const wchar_t* what, bool reads = false);   // research: who writes there   (watch.cpp)
void NetplayWatchAim(int player);      //   research: who writes a player's aim point (test pipe "watchaim")                   // research: the game's random numbers   (rngtrace.cpp)
void InitSplitView();                  // one full-screen view per PC online    (splitview.cpp)
int SplitViewHudMode(int mode);
bool NetplayStoryCoop();               // netplay.cpp: this session is story co-op
void InitReports();                    // report.cpp: zips of the logs - crashed games found at start
void ReportDesync(unsigned tick, const char* what);   //   the match went out of sync: this PC's report
int ReportCount();                     //   FGUYMOD_ReportCount
const char* ReportNew();               //   FGUYMOD_ReportNew: one saved in the last match ("" none)
void ReportOpenFolder();               //   FGUYMOD_OpenReports
DWORD HandoffLobbyPid();               // handoff.cpp: the session game's lobby game (0: none)
std::string NetplayStats();            //   FGUYMOD_NetStats: "name:ping ms|..." of the other players
bool OnlineShowPing();                 // onlinesettings.cpp: the match shows the pings
void VideoHudShown(bool shown);        // d3d9.cpp: the level's HUD is showing (the chosen frame rate; else 60 at most)
bool OnlineDirectConnection();         //   direct connections to players who have them on too (netplay.cpp)
void SplitViewHudHas4Way(bool has);    //   the level's HUD: does it have the 4-way layout? (the story's has none)
int SplitViewMyViewport();             //   online: my player's viewport (-1: offline / not in the level yet)
int SplitViewShownViewport();          //   the one shown (spectating: a teammate's)
void SplitViewPredict(bool on, float turn, float x, float z);   //   netplay: my view's turn still to come (display only)
unsigned NetplayShownTick();           // netplay.cpp: the tick the frame being drawn shows
float NetplayModelTurn(unsigned from, unsigned to);   //   the model's turn of my inputs of ticks (from, to]
bool OnlinePauseMenusUp();             // onlinepause.cpp: a menu has my input (mine, the game's pause, a HUD menu)
void SplitViewSpectateTick(int rate);  //   netplay, every tick: dead in Multiverse Madness - spectate
const wchar_t* SplitViewSpectating();  //   FGUYMOD_Spectating: whose view ("" mine)
float NetplayPlayerHealth(const unsigned char* player);   // netplay.cpp: a player's health (0: down)
bool OnlineSpectate();                 // onlinesettings.cpp: SETTINGS > Spectate when dead
// modloader.cpp: separate mods (FGUYMod\mods\<folder>, sdk\fguymod_plugin.h)
void InitMods();                       //   at start: the mods there are, which are on
void ModsLoadPlugins();                //   at the end of start-up: their plugins
bool ModsFileOverride(const wchar_t* gamePath, wchar_t* out);   //   a mod's copy of a game file
std::vector<std::wstring> ModsUiFolders();   //   their movie patches' folders
void ModsTick();                       //   vpad.cpp: a game tick starts (the game's thread)
void ModsFrame();                      //   d3d9.cpp: a frame was drawn
std::string ModsOnlineKey();           //   the gameplay mods that are on (the lobby server matches by it)
const char* ModsList();                //   FGUYMOD_ModsList (the MODS page)
bool ModsToggle(const char* folder);   //   FGUYMOD_ModsToggle
void ModsOpenFolder();                 //   FGUYMOD_ModsOpenFolder
struct KbmRaw;
bool NetplayInputOf(int slot, WORD* buttons, BYTE* lt, BYTE* rt, SHORT* sticks, KbmRaw* kbm, bool* keyboard);
const char* ModVersion();              // online.cpp: the mod's version
void InitUpdater();                    // updater.cpp: the player's own game checks for a newer release
const char* UpdateState();             //   FGUYMOD_UpdateState
void UpdateInstall();                  //   FGUYMOD_UpdateInstall
int SplitViewHudViewport(int viewport);
int SplitViewHudZoomViewport();        //   online: the HUD quarter to show full screen (-1: none)
void SplitViewMenuCheck();
const char* SplitViewMenuCamera();   // FGUYMOD_MenuCamera: the menus' 3D camera position, "x|y|z"             //   online.cpp: left the ONLINE screens (research / panel reset)
void SplitViewMenuRect(float left, float top, float right, float bottom);  // lobby: 3D scene inside a panel

// onlinemenu.cpp: keyboard / mouse helpers for the ONLINE screens, and the test bot
void InitOnlineMenu();                 // onlinemenu.cpp: the Store's own key turning off on the ONLINE screens
void OnlineMenuOpen(bool open);
void OnlineMenuKey(HWND wnd, WPARAM vk);    //   display.cpp: a key went down in the game window
const char* OnlineMenuTyped();
bool OnlineMenuFromWASD(int key);
void OnlineMenuTurnPreview(float radians);
void OnlineMenuChar(WPARAM ch);        //   the lobby's chat: a typed character (WM_CHAR)
void OnlineMenuChatMode(bool on);      //   FGUYMOD_ChatMode
const char* OnlineMenuChatTyped();     //   FGUYMOD_ChatTyped
void OnlineMenuFrame();                     //   d3d9.cpp, every frame
bool IsBot();                               //   this copy is the test bot (FGUYMOD_BOT)
bool StaysMuted();                          //   the bot / test copies: no sound
const char* BotJoinCode();

// onlinesettings.cpp: every player's own settings in an online match
struct KbmRaw;
void OnlineSettingsInit();                      // session game: what the player's own game had (env)
void OnlineSettingsStamp(KbmRaw* mine);         // netplay: my aim / invert / reticles go with my input
void OnlineSettingsApply(const KbmRaw* bySlot, int players);   // netplay, every tick: everyone's, per player
void OnlineSettingsProfileLoaded();             // session game: my sound / subtitles over the host's save
void OnlineSettingsReport(const char* s);        // lobby game: FGUYMOD_MySettings (the game's own getters)
std::string OnlineSettingsCapture();            // lobby game, starting a match: mine, for the session game
void OnlineSettingsWriteBack(DWORD lobbyPid);   // session game, handing back: mine as they are now
const char* OnlineSettingsBack();               // lobby game, back from a match: the menu applies these
bool OnlineVibration();                         // my controller may rumble ([Online] Vibration)
// The SETTINGS page of the online pause menu (onlinepause.cpp draws / drives it)
struct SettingsRow { const char* label; std::string value; char kind; };   // kind: h header, s slider,
int OnlineSettingsRows(SettingsRow* out, int max);                         //   t toggle, c choice, i info, b back
void OnlineSettingsChange(int row, int delta);  //   left / right (delta -1 / +1), A (0)

// overlay.cpp: a notice over the game window (online: waiting for a player, out of sync)
void OverlayShow(const wchar_t* text, DWORD ms);   // ms 0: until OverlayHide
void OverlayHide();

// onlinepause.cpp: the online pause menu (the match keeps running; the game pauses once everyone has it open)
struct KbmRaw;
bool OnlinePauseSample(WORD* buttons, SHORT* lx, SHORT* ly, KbmRaw* kbm, bool keyboardPlayer);  // netplay: my input
int OnlinePauseTick(const BYTE* flags, const bool* gone, int players, bool* keyboard); //   everyone's
void OnlinePauseCommand(const char* command);  //   ui.cpp: a menu command ran
const char* OnlinePauseMenu();                 //   FGUYMOD_PauseMenu (the HUD)
const char* OnlinePauseCursor();
const char* OnlinePauseDeathMouse(const char* boxes);   //   FGUYMOD_DeathMouse (the HUD): my death menu's buttons
void OnlinePauseHudTakesStart(bool up);                 //   FGUYMOD_HudTakesStart (the HUD): my weapons screen is up
const char* OnlineMenuMouse(const char* boxes, const char* who);   //   FGUYMOD_MenuMouse (the game's menus): their lists
void OnlinePauseSettings();                     //   FGUYMOD_PauseSettings (the game's pause menu: OPTIONS)
const char* OnlinePauseRematch();               //   FGUYMOD_Rematch (the end of the match: the votes)
bool OnlinePauseBotVotes();                     //   the test bot's scripted input votes for the rematch
void OnlinePauseQuitDone();
void OnlinePauseForceQuit();                    //   netplay: out of sync - everyone back to the lobby

// online.cpp: lobby + the in-game ONLINE screens
void OnlineEnter();
void OnlineRejoin(const char* code, const char* token, int slot);   //   after a match: back to its lobby
void OnlineExit();
const char* LobbyState();
void LobbyDo(const char* command, const char* a, const char* b, const char* c, const char* d, const char* e);
const char* SessionConfig();
bool OnlineStartingMatch();   // the online loading screen shows while the match's game boots hidden

// display.cpp
bool DisplayIsWindowed();              // windowed or borderless right now
void DisplayRenderSize(int* w, int* h); // the game's render size
void DisplayClientToGame(POINT* pt);   // window client pixels -> the game's (borderless render size)
int DisplayMenuIndex();                // Options > Screen > Display Mode support
bool DisplayMenuSelect(int index);     //   returns the game's own "windowed" flag for that choice
void DisplayMenuApply();
void DisplayMenuReset();
bool DisplayOnChangeResolution(int* width, int* height, bool windowed);  // may change the size (borderless)
void DisplayReveal();                  // show the hidden online-session window
bool DisplayWindowRect(RECT* r);       // where the game window is (handed to the session game)
void DisplayShowForQuit();             // test tools' quit: bring the window up so it draws its last frame
void DisplayHideWindow();              // hide the window (old game handing over to the session)
void DisplayFullscreenFailed();        // d3d9.cpp: Direct3D refused exclusive fullscreen - borderless instead
void DisplayCheckStyle();              // d3d9.cpp, every frame: no title bar while borderless
void DisplayAfterReset();              // d3d9.cpp: borderless again if a Reset brought the frame back
bool DisplayPark();                    // the lobby game during its player's match: hidden, held (online.cpp)
void DisplayUnpark(bool show, const RECT* at, HANDLE shown);   //   go on (shown: at `at`), then set `shown`
bool DisplayWindowRectOf(DWORD pid, RECT* r);   // another process's game window (the session game's)
HWND DisplayMainWindow();              // the game's window
bool DisplayOsPointer(bool want);      // online menus: the Windows mouse pointer shown (false: draw your own)

// handoff.cpp: the old game shows the loading screen while the session boots hidden
void InitHandoff();
bool HandoffHidden();                  //   this game is a session booting hidden
void HandoffMatchRunning();            //   the match is running: show it
void HandoffRevealed();
bool HandoffHudUp();                   //   this game's level HUD has laid itself out (ui.cpp)
bool HandoffBooting();                 //   booting hidden, the match not running yet
void HandoffMute(bool mute);           //   the lobby game waiting hidden: its sound off / on
void HandoffMuteInWindows();           //   research: this process muted in Windows' mixer
bool HandoffGiveBack();                //   the match is over: back to the player's own (lobby) game
int HandoffGiveBackState();            //   0 none (the lobby opens here), 1 handing back, 2 handed back
bool HandoffCatchingUp();              //   this game rejoins a running match and is still catching up (netplay.cpp)
void HandoffCaughtUp();                //   netplay.cpp: caught up - show the match

// Rejoining a running match (netplay.cpp catches up; the lobby game, online.cpp, shows how far it is): the session
// game shares this as Local\FGUYMod_CatchUp_<its process id>.
struct CatchUpShared {
    volatile LONG tick, hostTick;   // where the catching-up game is, and the match
    volatile LONG state;            // 0 catching up, 1 caught up, 2 failed (why: reason)
    wchar_t reason[96];
};

// kbm.cpp: the game's own keyboard / mouse controls in online sessions
#pragma pack(push, 1)
struct KbmRaw {            // one player's raw keyboard and mouse for one tick (sent with their inputs)
    BYTE keys[32];         //   key down bits, by Windows key code
    SHORT dx, dy, dz;      //   mouse movement and wheel
    BYTE buttons;          //   mouse buttons (bit 0 left, 1 right, 2 middle, 3/4 side)
    SHORT cursorX, cursorY; //   mouse cursor in the window, 0..32767 across / down (every PC's window differs)
    BYTE flags;            //   bit 0: this player's level HUD is up (every player's: keyboard mode starts)
    BYTE aim;              //   this player's aim sensitivity, 0..100 (onlinesettings.cpp)
    BYTE opts;             //   bit 0 invert look, bit 1 reticles, bit 7: aim / opts set
    BYTE pace;             //   frames per second this player's PC manages right now (0: not known) - netplay.cpp
    BYTE sync;             //   (the host) 1: everyone takes the host's random numbers at this tick - netplay.cpp
};
#pragma pack(pop)
void KbmInit(HWND wnd);
bool KbmOnMessage(HWND wnd, UINT msg, WPARAM wp, LPARAM lp, LRESULT* result);
bool KbmSkipPressed();
void KbmSessionStart(const char* session);     // netplay.cpp: which players play with keyboard and mouse
void KbmLeaveSession();                         //   netplay.cpp: the session is over for this game
bool KbmIsKeyboardSlot(int slot);
void KbmLevelMenu(int player, bool open);      //   ui.cpp: a level menu takes / gives back a player's input (CTRL_Add/RemoveInput)
bool KbmCursorScreen(POINT* pt);               //   netplay.cpp: the keyboard players' cursor this tick, on screen
void KbmCapture(KbmRaw* out);                  //   my keyboard and mouse now
void KbmApply(const KbmRaw* bySlot, int players);  //   every tick: feed the keyboard players' devices
void KbmBackInLobby(bool keyboard);               //   ui.cpp: back from a match - this player's controls again
void KbmTestCommand(const char* what, int a, int b, int c);  // test pipe: kbm key <vk> <ms> | mouse <dx> <dy> <ticks> | click <ms>

// sounds.cpp: loading-screen music
void InitAudio(HMODULE exe);            // volume curve, movie volume            (audio.cpp)
void AudioProfileLoaded();             //   the main menu opened: the profile's settings are in
void AudioTitleScreen();               //   back at the title screen: movies & title volume again
void AudioSilenceGame(bool on);        //   online loading screen: the game's own sound off (online.cpp)
void AudioTick();                      //   the game thread, every tick (vpad.cpp)
float AudioGameVolume(int which);      //   Options > Audio: 0 music, 1 effects, 2 dialog, 3 cinematics (0..1)
void AudioSetGameVolume(int which, float v);
float AudioMusicVolume();              //   0..1, for our own sounds
float AudioMovieVolume();              //   Options > Audio > MOVIES & TITLE, 0..1
void AudioSetMovieVolume(float v);
void InitSounds();
void SoundsOnFileOpen(const wchar_t* path);       //   config.cpp: a file the game opens
void SoundsOnlineLoading(const char* mode, const char* map);  // the lobby's loading screen
