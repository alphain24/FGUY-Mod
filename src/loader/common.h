#pragma once
// Shared bits for the loader's modules.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <memory>
#include <string>
#include <vector>

extern wchar_t g_gameDir[MAX_PATH];  // folder containing FGUY.exe
extern wchar_t g_modDir[MAX_PATH];   // <game>\FGUYMod

void Log(const wchar_t* fmt, ...);
// (in a block: how long it took, in the log when it's over `ms` - a stall's cause, in players' logs too)
struct SlowLog {
    const wchar_t* what;
    double ms;
    LARGE_INTEGER t0;
    explicit SlowLog(const wchar_t* w, double limit = 20) : what(w), ms(limit) { QueryPerformanceCounter(&t0); }
    ~SlowLog() {
        LARGE_INTEGER t1, f;
        QueryPerformanceCounter(&t1);
        QueryPerformanceFrequency(&f);
        double took = (t1.QuadPart - t0.QuadPart) * 1000.0 / f.QuadPart;
        if (took > ms) Log(L"Slow: %s took %.0f ms (thread %lu)", what, took, GetCurrentThreadId());
    }
};
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
void InitSteadyClock(HMODULE exe);     // offline: the game's clock one frame a tick (steady.cpp)
bool SteadyClockOn();
void SteadyTick();                     //   a controller poll (the first of a tick starts it)
void SteadySetInterval(LONGLONG counts);   //   the frame interval the pacing holds (d3d9.cpp)
void VideoRequestQuit();               //   exit cleanly at the end of the next frame (test tools)
void VideoRequestShot(const wchar_t* path);   // test tools: save the next frame as a .bmp
void VideoRequestShots(int frames, const wchar_t* folder);   //   ...the next n, every one, small (frame_<i>.bmp, times.txt)
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
int KbmKeyPlayer(int code);                          //   the HUD's key event: whose (-1: as the game has it)
void ModsTestHook(unsigned address, int length);    //   test pipe "hooktest <address> <length>": the mods' Hook, tried
void NetplayTestGod(bool on);
void NetplayTestSendEvent(unsigned lo, unsigned hi, unsigned event);   //   test pipe "sendevent": an object gets an event
void NetplayStoryLevelChange(const char* what);   // ui.cpp: the menu restarts the level / goes to another (story co-op)                         //   test pipe "god 0/1": my player takes no hits
void NetplayTestPointOn(unsigned id);                 //   test pipe "pointon <id>": the host's control point on
void NetplayTestKillNpcs();                          //   test pipe "killnpcs": the host's enemies all down
void NetplayTestHitNpc(unsigned id, int amount);     //   test pipe "hitnpc <id> <amount>": my player hits that enemy
void NetplayTestFreeze(int ms);                      //   test pipe "freeze <ms>": the game loop stops a while
void NetplayTestCommand(const char* command, const char* args);   //   test pipe "uicmd <command> [args]"
bool UiRunCommand(const char* command, const char* args);   // ui.cpp: a menu command of the mod's own (game thread)
void NetplayLevelFileOpened(const wchar_t* path);   // config.cpp: a file opened (a level's package: story co-op)
void NetplayFetchSave(const wchar_t* path);         // config.cpp: the save about to be read - rejoining story: the host's
void NetplayTestHurt(int player, float amount);   //   test pipe "hurt": my player there takes a hit nobody caused
void OnlineMenuLogYaw();               //   test pipe "yaw": the lobby preview's facing (onlinemenu.cpp)
void NetplayOfflineTick();                  // vpad.cpp: every pad 0 poll outside sessions (test pipe's resolution)
void NetplayTestResolution(int w, int h, bool windowed = true);   //   test pipe: the game changes resolution (Options > Screen)             //   test pipe: my player moves a little here only (out of sync)
int NetplaySessionSlot();              //   my player slot in this game's session (also after leaving it)
unsigned NetplayTick();                //   the lockstep tick being simulated
int NetplayRate();                     //   ticks (frames) per second online
void NetplayLogStacks();               //   research (test pipe "stacks")
bool NetplayLevelLive();               //   session: every player's level HUD is up (same tick everywhere)
void NetplaySessionPress(int pad, WORD buttons, int delayTicks);  // session autopilot
void NetplaySessionLevel();            //   the menu launched the level
void NetplayLeave(const wchar_t* why); //   leave the session (back to the lobby): run on our own
bool NetplaySessionReturn();
void NetplaySessionLeaveAsked();      //   story co-op: the main menu's ONLINE - back to the lobby
bool NetplaySessionStarted();         //   a level of this session ran already (the menus: no autopilot again)           //   the menus: the match is over - back to the lobby (leaves the session)
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
bool VideoHudUp();                     //   it showed just now (a level is being played)
bool OnlineDirectConnection();         //   direct connections to players who have them on too (netplay.cpp)
void SplitViewHudHas4Way(bool has);    //   the level's HUD: does it have the 4-way layout? (the story's has none)
int SplitViewMyViewport();             //   online: my player's viewport (-1: offline / not in the level yet)
int SplitViewShownViewport();          //   the one shown (spectating: a teammate's)
int SplitViewDrawing();                //   render thread: the view drawing now (-1: another camera's)
bool SplitViewRect(int viewport, float* ltrb);   //   a view's place on the screen (0..1) - false: not shown
void SplitViewPredict(bool on, float turn, float x, float z);   //   netplay: my view's turn still to come (display only)
unsigned NetplayShownTick();           // netplay.cpp: the tick the frame being drawn shows
float NetplayModelTurn(unsigned from, unsigned to);   //   the model's turn of my inputs of ticks (from, to]
bool OnlinePauseMenusUp();             // onlinepause.cpp: a menu has my input (mine, the game's pause, a HUD menu)
bool OnlinePauseGamePaused();          //   story co-op: the game's own pause menu is up (everyone's paused)
void SplitViewSpectateTick(int rate);  //   netplay, every tick: dead in Multiverse Madness - spectate
const wchar_t* SplitViewSpectating();  //   FGUYMOD_Spectating: whose view ("" mine)
float NetplayPlayerHealth(const unsigned char* player);   // netplay.cpp: a player's health (0: down)
bool OnlineSpectate();                 // onlinesettings.cpp: SETTINGS > Spectate when dead
// modloader.cpp: separate mods (FGUYMod\mods\<folder>, sdk\fguymod_plugin.h)
void InitMods();                       //   at start: the mods there are, which are on
void ModsLoadPlugins();                //   at the end of start-up: their plugins
bool ModsFileOverride(const wchar_t* gamePath, wchar_t* out);   //   a mod's copy of a game file
std::vector<std::wstring> ModsUiFolders();   //   their movie patches' folders
std::vector<std::pair<std::wstring, std::wstring>> ModsContentFolders(const wchar_t* sub);   //   <mod>\<sub> of those on, then FGUYMod\<sub>: (folder, mod name)
void ModsTick();                       //   vpad.cpp: a game tick starts (the game's thread)
void ModsFrame();                      //   d3d9.cpp: a frame was drawn
struct IDirect3DDevice9;
void CosmeticsFrame(IDirect3DDevice9* dev);   // cosmetics.cpp: skins, on the render thread after each frame
void CosmeticsTest(bool fill, unsigned lo, unsigned hi, unsigned rgb);   //   test pipe "texinfo" / "texfill"
void CosmeticsDeviceCreated(IDirect3DDevice9* dev);   //   d3d9.cpp: the game's device
// graphics.cpp: sharp picture, texture filtering, anti-aliasing, sharpening, brightness (FGUYMod.ini [Graphics])
void GraphicsLoadSettings();
void GraphicsDeviceCreated(IDirect3DDevice9* dev, UINT w, UINT h);
void GraphicsBeforeReset();
void PoolTraceInstall(IDirect3DDevice9* dev);   // research FGUYMOD_TEST_POOLTRACE: the D3DPOOL_DEFAULT resources alive
void PoolTraceDump();                           //   (pooltrace.cpp - on a refused Reset)
void GraphicsAfterReset(IDirect3DDevice9* dev, UINT w, UINT h);
void GraphicsPresent(IDirect3DDevice9* dev);   //   every frame, before it's shown
bool GraphicsMenuNew();                        //   the main menu's SETTINGS page: Options opens it (else the game's)
const char* GraphicsMenuRows(int page);        //   its rows ("label~value~kind~hint\n...")
int GraphicsMenuChange(int page, int row, int delta);   //   a row changed: 1 open the game's Screen page, 2 its Options
struct IDirect3DTexture9;
struct IDirect3DIndexBuffer9;
bool SkinBuildTexture(IDirect3DTexture9* game, const std::vector<double>& ops, const std::vector<unsigned char>& overlay,
                      std::vector<unsigned char>& dds);   // skinbuild.cpp: a skin's texture from the game's own (format 2)
bool SkinPngToDds(const std::vector<unsigned char>& png, std::vector<unsigned char>& dds);   //   a PNG as a DXT5 DDS, its mips
void SkinRgbaToDds(const std::vector<unsigned char>& rgba, unsigned w, unsigned h, std::vector<unsigned char>& dds);   //   ... pixels
bool SkinDecodePng(const std::vector<unsigned char>& png, std::vector<unsigned char>& rgba, unsigned* w, unsigned* h);   //   its pixels
bool SkinBuildRgba(IDirect3DTexture9* game, const std::vector<double>& ops, const std::vector<unsigned char>& overlay,
                   std::vector<unsigned char>& rgba, unsigned* w, unsigned* h);   //   a skin's picture made here
enum _D3DFORMAT;
bool SkinReadTexture(IDirect3DTexture9* game, std::vector<unsigned char>& rgba, unsigned* w, unsigned* h, _D3DFORMAT* fmt,
                     unsigned* mips);   //   HD character textures: the game's (managed) texture as RGBA, render thread
void SkinBuildFrom(std::vector<unsigned char> px, unsigned w, unsigned h, _D3DFORMAT fmt, unsigned mips,
                   const std::vector<double>& ops, const std::vector<unsigned char>& overlay,
                   std::vector<unsigned char>& dds);   //   a skin's texture from the game's pixels (any thread)
void SkinUpscaleDds(const std::vector<unsigned char>& rgba, unsigned w, unsigned h, _D3DFORMAT fmt, unsigned mips,
                    std::vector<unsigned char>& dds);   //   ... twice its size, as a DDS (any thread)
void GraphicsFrameLog(int frames);   // research: each frame's shaders in order (with FGUYMOD_TEST_PSTRACE)
void FirstPersonPoseStart(unsigned char* entity);   // their pose about to be built: turned the way they look, for the drawing
void FirstPersonUnturn();   // a tick starts: the game's own transforms back (first person turned them for the drawing)
void GraphicsAimNearBlur(bool on);   // research: the aimed view's near blur left on in first person
bool GraphicsHdCharacters();   // graphics.cpp: Graphics > HD character textures
bool GraphicsDrawingInk();     //   the ink strokes' shader is the one set
bool GraphicsDrawingOutline(); //   a character's outline shell's shader is the one set
// icons.cpp: skin icons in the menus
std::string IconsAdd(const std::vector<unsigned char>& png);   // the movie a menu loads to show it ("fguyicon_<key>.gfx"; "": not a PNG)
const std::vector<unsigned char>* IconsMovie(const char* url);  //   ui.cpp: the movie for a file the game opens (nullptr: not one)
void IconsAddPixels(const std::string& key, const std::vector<unsigned char>& rgba, unsigned w, unsigned h);   //   a picture made here
bool IconsReady(const std::string& url);   //   its movie can be made
std::string CosmeticsMySkinKey();               // cosmetics.cpp: the id of my lobby skin's head picture ("" none)
std::string CosmeticsSkinHead(const std::string& key);   //   that head picture here: "<movie>|l|r|b" ("": no pack of it here)
const char* CosmeticsOfflineSkins();             // cosmetics.cpp, the game's own skin pickers offline (costumes.as2js)
void CosmeticsOfflineSkinPick(int player, const char* file);
const char* CosmeticsOfflineSkinFor(int player, const char* pic);
const char* CosmeticsStoreSkinOf(const char* pic);
void CosmeticsStoreSkinPick(const char* pic, const char* file);
const char* CosmeticsHeadIcon(int player);   // cosmetics.cpp, FGUYMOD_HeadIcon: the HUD head of the skin a player wears ("" none)
IDirect3DTexture9* CosmeticsTextureFromDds(IDirect3DDevice9* dev, const std::vector<unsigned char>& dds);   // cosmetics.cpp
int CosmeticsDrawPlayer();             //   which player this draw is of (-1 none)
bool CosmeticsOwnBodyClipped();        //   this draw is a player's own body in first person, clipped (no outline)
bool CosmeticsArmInk();                //   this draw is first person's arms, inked (no outline of its own)
void CosmeticsArmInkTest(float px, float bias, float slope);   //   research: "fpv ink <px> <bias> <slope>"
// models.cpp: new characters (the FGUY Cosmetics mod's models)
using ModelsRealDraw = HRESULT(__stdcall*)(IDirect3DDevice9*, int, int, unsigned, unsigned, unsigned, unsigned);
void ModelsLoad(int slot, const std::vector<unsigned char>& pack, const wchar_t* name);   // a player wears a model ({}: none)
void ModelsLoadFile(int slot, const wchar_t* path);   //   test pipe "modelload <slot> <file.fgmodel>"
bool ModelsWorn();
void InitModels();                     //   at start: hooks (their ink lines off)
void ModelsResearch(int slot);         //   test pipe "modelpieces <slot>"
void ModelsFrame(IDirect3DDevice9* dev);   //   cosmetics.cpp, after each frame
bool ModelsDraw(IDirect3DDevice9* dev, int player, ModelsRealDraw real, HRESULT* hr, IDirect3DIndexBuffer9** putBack);   //   a draw of theirs: ours instead
void CosmeticsTrace(unsigned lo, unsigned hi);   //   test pipe "texdraws": a frame's draws with that texture
void InitFirstPerson();                // a player's camera at their eyes                     (firstperson.cpp)
void FirstPersonCommand(const char* args);   //   test pipe "fpv <player> <0|1> [height] [forward] [pitch limit]"
bool FirstPersonAny();                 //   someone is in first person
bool FirstPersonMine();                //   onlinesettings.cpp: my player in first person (goes with my input)
bool FirstPersonDefault();             //   graphics.cpp (SETTINGS > CAMERA): players start in first person
void FirstPersonDrawnCamera(int view, float* m);   //   splitview.cpp: a view's camera as the game hands it (+0x94) - emoting: from behind
bool FirstPersonNoLines(float x, float y, float z);   //   models.cpp: no ink lines for what's there, in the view drawing now
bool MotionPlaying(int slot);          //   (motion.cpp)
void FirstPersonSetDefault(bool on);   //     (kept in FGUYMod.ini)
const char* FirstPersonFovSlider();    //   graphics.cpp (SETTINGS > CAMERA): first person's field of view, its slider's row
void FirstPersonFovStep(int by);       //     a step along it (kept in FGUYMod.ini)
int FirstPersonFovMine();              //   onlinesettings.cpp: mine, as it goes with my input (its code, 1..13)
void FirstPersonFovFromInput(int player, int code);   //   ...and a player's from theirs
bool FirstPersonCrosshair();           //   graphics.cpp (SETTINGS > CAMERA): a crosshair in first person
void FirstPersonSetCrosshair(bool on); //     (kept in FGUYMod.ini)
const char* FirstPersonCrosshairs();   //   FGUYMOD_Crosshairs: where the HUD draws them
void FirstPersonFromInput(int player, bool on);   //   onlinesettings.cpp, every tick: that player's, from their input
int FirstPersonOwnDraw(const float (*c)[4], float* planes);   //   render thread (cosmetics.cpp): a player's own body in their view - clip planes (how many), -1 skip
void InitMotion();                     // characters' poses: read / ours instead              (motion.cpp)
void MotionFrame();                    //   d3d9.cpp: a frame was drawn
void MotionCommand(const char* cmd, const char* a, const char* b);   //   test pipe "poserec" / "posefreeze" / "poseplay" / "posestop" / "findptr"
struct MotionClip;                     //   a motion (motion.bin)
std::shared_ptr<const MotionClip> MotionParse(const unsigned char* data, size_t size);
int MotionBones(const MotionClip& m);
float MotionLength(const MotionClip& m);
void MotionPlay(int slot, std::shared_ptr<const MotionClip> clip, const std::vector<bool>& uses, bool loop);   //   on a player
void MotionStop(int slot);             //   (blending back to the game's own)
bool MotionPlaying(int slot);
bool MotionPose(int slot, std::vector<float>& out, int* bones);   //   a player's pose this frame (models.cpp)
bool MotionHead(int slot, float* at);  //   where a player's head is this frame, in the world (firstperson.cpp)
void MotionArmsTest(float right, float down, float ahead, bool on);   //   research: the gun hand's place in first person
void MotionArmsZoomTest(float right, float down);                     //     (zoomed in)
void MotionArmsBodyTest(bool hide);                                   //   research: the rest of the body hidden or not
void MotionGunAimTest(bool on);                                       //   research: a two-handed gun pointed at the crosshair
void MotionOrderTest(int n);                                          //   research: poses and frames in order
int MotionOrderTrace();
void MotionGameBobTest(float up, float side);                       //   research: the gun's bob from the head's
void MotionArmMoves(int mode, float swing, bool log);                 //   [Camera] FirstPersonMoves: their arms on the move
void MotionArmSize(float size);                                       //   [Camera] FirstPersonArmSize: how big they look (1: as set)
void MotionArmFlat(float flat);                                       //   [Camera] FirstPersonArmFlat: their perspective (1: their own)
void MotionArmCalm(float calm);                                       //   third person's arms on the move: how calm (0..1)
void MotionOffBones();                                                //   research: the off hand's bones, logged once
void MotionRig(const float* values, int count);                       //   research: "fpv rig <hands ahead> <shoulder right> <down> <ahead> <shrink> <kept ahead>"
void MotionFingers(const float* values, int count);                   //   research: "fpv fingers <relaxed> <tip> <grip> <tip> <fist> <tip>"
void MotionLess(float reload, float walk);                            //   research: "fpv less <reload> <walk>"
void MotionGripThumb(const float* v);                                 //   research: "fpv thumb <x y z> <x y z>"
void MotionKick(const float* values, int count);                      //   research: "fpv kick <back> <up> <pitch> <yaw>"
void MotionShotLog(bool on);                                          //   research: "fpv shots 1"
void MotionHideDown(float y, float ahead);                                         //   research: "fpv hidedown <y>"
void MotionPunchAim(const float* values, int count);                  //   research: "fpv punch <up> <in> <twist> <thumb +-1>"
void MotionArmPose(int stance, const float* values, int count);       //   research: Peter's pose for a stance (motion.cpp g_peterPose)
void MotionArmsWhyTest(bool on);                            //   research: why the arms aren't first person's
void MotionBobTraceTest(int poses, bool view);                                 //   research: the game's own bounce, each pose
void MotionOffHandTest(bool on);                                  //   research: the off hand put on a one-handed gun
void MotionArmsTraceTest(int frames);                               //   research: the arms' pose against each frame drawn
void MotionArmsTrace(int slot, const float* eyeDraw, const float* fwDraw);   //     (firstperson.cpp: this frame's camera)
void MotionArmsScaleTest(float s);                                //   research: the arms shrunk toward the eye
void MotionArmsDevTest(float mix, bool log);                        //   research: the gun hand kept on its hold between stances
bool MotionArmsInView(int slot);       //   their gun was brought into their view this frame (their head shrunk away)
struct ArmsView {                      // a player's gun to bring into their view (FirstPersonArms, for motion.cpp):
    float eye[3], forward[3];          //   their eye and which way they look (the world)
    float weight;                      //   how far (0..1)
    bool still;                        //   standing still
    float tans[2];                     //   their view's half widths (tan across, up)
    float zoom;                        //   their weapon's zoom (0 none .. 1 all the way)
    float wide;                        //   ...their view's width with it against without (1: not zoomed)
    float aim, rest;                   //   the game's upper body: its aiming stance's weight, its relaxed one's (both
                                       //   low: something else - a melee swing, a jump, a power-up)
    bool alone;                        //   theirs the only view on the screen (split screen: the partner sees them)
    int action = -1;                   //   the upper body's action played over its stances ([+0x13cc]+0x78; -1 none)
    float idle = 0;                    //   an idle fidget's weight (stood still a while: 0..1)
    int layer90 = 0;                   //   (research: its state)
    int clip = 0;                      //   the upper body's clip ([+0x13cc]+0x60: 0 none, 0xb aiming, 4..6 reloading)
    int moveClip = 0;                  //   ...and what's under it (+0x5c: 0 standing, 2 walking, 4 running, 0x16 in the air)
    bool gunOut = true;                //   their gun's out (not: a melee swing, the hands empty after one)
    int rounds = 0;                        // (the clip: player +0x4ffc)
};
bool FirstPersonArms(int slot, ArmsView* out);
int FirstPersonArmsNoWhy();            //   research: why FirstPersonArms last said no (0: it didn't)
const float kArmTan = 0.79f;           // first person's arms drawn with a field of view of their own: tan across (the
                                       //   game's 80 degrees at 16:9) - the same size at any field of view, zoomed too
bool MotionViewmodel(int slot, float* hand);   //   render thread: their arms drawn as first person's (their body hidden
                                               //   away) - their gun hand in the world, [3] how far shrunk to the eye,
                                               //   [4..9] the eye and the view's way ahead they were posed for,
                                               //   [10..12] the sway and bob posed (the view's: left, up, ahead),
                                               //   [13..15] the zoom they're drawn at and its spot on the screen
bool MotionOffsetAt(int slot, double t, float* out);   //   ...their sway and bob at time t (the PC's clock, s)
void FirstPersonFrameEnd();             //   d3d9.cpp: a frame's been drawn (research: the gun's pieces counted)
bool FirstPersonShellInner();          //   render thread (cosmetics.cpp): a shell's part within the arms' reach is drawn as them
bool FirstPersonInked();               //   ...this draw's arms and gun have ink of their own, no shells
bool FirstPersonSkipDraw();             //   render thread (cosmetics.cpp), after FirstPersonViewmodel: this draw's left out
bool FirstPersonShell(const float (*c)[4], float (*inner)[4], float* nearPlane, float* farPlane);   //   cosmetics.cpp:
                                       //   an outline shell, split at first person's arms' reach (FirstPersonShell)
bool FirstPersonViewmodel(const float (*c)[4], bool skinned, float (*out)[4]);   //   cosmetics.cpp: this draw is
                                       //   first person's arms or gun - drawn in front of all, with `out` for c0..c3 (with
                                       //   the camera, their own field of view, the near plane nearer)
struct _XINPUT_STATE;
void EmotesPad(DWORD pad, _XINPUT_STATE* st);   // emotes.cpp: held Back is the emote wheel     (emotes.cpp)
bool EmotesMouse(LONG* dx, LONG* dy, LONG* dz);  //   kbm.cpp: the mouse, the keyboard's wheel open (true: taken)
const char* EmotesWheelText();         //   FGUYMOD_EmoteWheel: what the HUD draws
void EmotesFrame();                    //   d3d9.cpp: a frame was drawn (B, the wheel, starting / stopping)
void EmotesRequest(int slot, int nth); //   test pipe "emote <slot> <n>"
void EmotesTestKey(bool down, int dx, int dy);   //   test pipe "emotekey <0|1> [dx dy]"
bool EmotesLocalPack(unsigned crc, std::vector<unsigned char>& out);   //   netplay.cpp: that pack, if it's here too
void EmotesRemote(int slot, const std::vector<unsigned char>* pack);   //   netplay.cpp: another player's emote (nullptr: stop)
void NetplayEmote(const std::vector<unsigned char>* pack);   // netplay.cpp: I play an emote (nullptr: stop) - everyone sees it
bool ZipRead(const std::vector<unsigned char>& zip, const std::string& name, std::vector<unsigned char>& out);   // cosmetics.cpp: a stored zip entry
bool ZipList(const std::vector<unsigned char>& zip, std::vector<std::string>& names);   //   its entries' names
void CosmeticsSetSkin(int slot, const std::vector<unsigned char>& pack, const wchar_t* name);   //   a player wears a skin pack
void CosmeticsLoadSkinFile(int slot, const wchar_t* path);   //   test pipe "skinload <slot> <file.fgskin>"
bool CosmeticsPackInfo(const std::vector<unsigned char>& pack, std::string* name, unsigned* lo, unsigned* hi);   //   a pack's name, its outfit
bool CosmeticsPlayerModel(int which, int nth, unsigned* lo, unsigned* hi);   //   the model (outfit) a player wears now
const char* CosmeticsSkinOptions();                //   FGUYMOD_SkinOptions: the skins for the lobby preview's outfit
void CosmeticsSkinPick(const char* file);           //   FGUYMOD_SkinPick: my skin for it
std::wstring CosmeticsMySkinPath();
struct SkinChoice { std::wstring file, path; std::string name; };
bool CosmeticsSkinChoices(int slot, std::vector<SkinChoice>* out, std::wstring* picked);   //   in a match: the skins
void CosmeticsRememberSkin(int slot, const std::wstring& file);                          //   for my outfit now
void NetplaySetMySkin(const std::wstring& path);   // netplay.cpp: mid-match, another skin pack of mine ("" none)
std::wstring NetplayMySkinPath();                  //   the one I wear ("" none)
const char* CosmeticsSkinList();                   //   FGUYMOD_SkinList: the MODS page's skins
void CosmeticsSkinToggle(const char* file);         //   FGUYMOD_SkinToggle: on / off for its outfit                 //   online.cpp: my skin pack for the match ("" none)
std::string ModsOnlineKey();           //   the gameplay mods that are on (the lobby server matches by it)
const char* ModsList();                //   FGUYMOD_ModsList (the MODS page)
bool ModsToggle(const char* folder);   //   FGUYMOD_ModsToggle
void ModsOpenFolder();                 //   FGUYMOD_ModsOpenFolder
bool ModsMenuCall(const char* name, const std::vector<std::string>& args, std::string& out, bool* answered);   //   FGUYPLUGIN_<name>: a plugin's
struct KbmRaw;
bool NetplayInputOf(int slot, WORD* buttons, BYTE* lt, BYTE* rt, SHORT* sticks, KbmRaw* kbm, bool* keyboard);
const char* ModVersion();              // online.cpp: the mod's version
void InitUpdater();                    // updater.cpp: the player's own game checks for a newer release
const char* UpdateState();             //   FGUYMOD_UpdateState
std::string LobbyServerFromRepo();     //   the mod's lobby server as the repo's server.txt names it ("" if never known)
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
void OnlineMenuWheel(int delta);       //   the window procedure: the mouse wheel turned
int OnlineMenuWheelSteps();            //   FGUYMOD_MenuWheel: its steps since (up: +)
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
void OnlinePauseCommand(const char* command, const char* args);  //   ui.cpp: a menu command ran
const char* OnlinePauseMenu();                 //   FGUYMOD_PauseMenu (the HUD)
const char* OnlinePauseCursor();
void SplitViewNoMarkers(bool on);
bool SplitViewCutscenePlaying();
bool SplitViewCutsceneRunning(unsigned char* cinematic);   //   that one is the game's current cutscene, running
bool SplitViewCutsceneSkippable();             //   ...and START would skip it               //   a cutscene of the game's playing (story)              //   FGUYMOD_DeathPanel: no name tags while my death menu's panel is up
const char* OnlinePauseDeathMouse(const char* boxes);   //   FGUYMOD_DeathMouse (the HUD): my death menu's buttons
void OnlinePauseHudTakesStart(bool up);                 //   FGUYMOD_HudTakesStart (the HUD): my weapons screen is up
const char* OnlineMenuMouse(const char* boxes, const char* who);   //   FGUYMOD_MenuMouse (the game's menus): their lists
void OnlinePauseSettings();                     //   FGUYMOD_PauseSettings (the game's pause menu: OPTIONS)
const char* OnlinePauseRematch();               //   FGUYMOD_Rematch (the end of the match: the votes)
bool OnlinePauseBotVotes();                     //   the test bot's scripted input votes for the rematch
void OnlinePauseQuitDone();
void OnlinePauseForceQuit();
bool OnlinePauseResultsShown();                 //   the end of the match's menu is up
bool OnlinePauseTakeLeave();                    //   a back-to-the-lobby asked (taken)
int NetplayResultsCommand(const char* command);    // netplay.cpp: the end of the match's pick, free-running (2: quit)
void NetplayRematchVoted();
bool NetplayResultsLeave();
const char* OnlineRematchPending();             // online.cpp: "restart" / "next" - back from a match for the next one
void OnlineRematchDone();                    //   netplay: out of sync - everyone back to the lobby

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
bool DisplayOnChangeResolution(int* width, int* height, bool* windowed);  // may change the size (borderless), fullscreen -> ours
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
    BYTE aim;              //   this player's aim sensitivity, 0..100 (onlinesettings.cpp); bit 7: the top of opts' code
    BYTE opts;             //   bit 0 invert look, bit 1 reticles, bit 7: aim / opts set; bit 2 first person (bit 3: said),
                           //   bits 4..6 its field of view's code (firstperson.cpp; aim's bit 7 above them)
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
void KbmCharacterScreen(int player, bool open);   //   ui.cpp (the HUD): a player's change character screen - W A S D as arrows
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
