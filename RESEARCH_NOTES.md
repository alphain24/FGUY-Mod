# Family Guy: Back to the Multiverse (PC) — Research Notes

Survey date: 2026-10-08. All findings from read-only inspection of the installed game.

## Executable (`FGUY.exe`)
- 32-bit x86, built 2012-11-12 with Visual C++ 2010 (`MSVCR100.dll`).
- SHA-256: `E4BCD19E208E2B17FF18E5B3BC37D6059FE8B4A6FC03CDDC0E86CE930F74E42B`
- Not packed or encrypted (normal section entropy, no SteamStub `.bind` section).
  Imports `steam_api.dll` normally: the Steam build of the game.
- Contains C++ RTTI class names (e.g. `.?AVNetworkModule@IO@@`) and source paths
  (`C:\branches\FGUY\main\NG\Source\Engine\...`) — very helpful for reverse engineering in Ghidra.
- Uses: Direct3D 9 + D3DX9_43, DirectInput8 + XInput 1.3, FMOD Ex (audio), Bink (video),
  Havok 2010.1 (physics), Scaleform (menus/UI), Winsock (`WS2_32.dll`: TCP + UDP).

## Leftover online networking code (key finding for online play)
The engine contains a network layer that the shipped game never exposed:
- Classes: `IO::NetworkModule`, `IO::Network::Comm::NetSystemWin32`, `IO::Network::Message::BaseMessage`,
  `zNetPlayer`, `zNetCommonPlayer`, `xNetProxy`, `zNPCNetworking`,
  `FGUYPlayerLocalAuthorityUpdateMessage` (game-specific, not just generic engine code).
- Log strings: `RUDP: SENDING '%d' (%d messages)` (reliable UDP), `[p2p] socket is closed.`,
  `Connecting state timeOuted - Server didn't respond`, `NO LONGER CONNECTED TO ANY NETWORK MACHINES.`
- Menu/script hooks: `EnableNetworkPlay`, `EnableNetworkSessions`, `EnableServerSearch`, `IsServer`,
  `NetworkLatency`, `SynchronizeScreen`, `SynchronizeVoteOptions`, `SynchronizeVoteSelection`.
- Command-line string: `-server=`
- Embedded default config (see below) has `HostIP`, `HostPort = 1000`, `IsServer = 0`, `EnableNetworkPlay = 0`.

Unknown yet: whether this code is complete/working, a dev-only test harness, or a half-finished cut feature.

## Embedded default config (`fguy.ini` / `boot.ini`)
The exe embeds a default `<game>.ini` and references `fguy.ini` and `boot.ini`. If the PC build reads
an external `fguy.ini`, these settings could be overridden without patching code. Notable keys:
- `BOOT = /FGUY/Levels/Level_FRXX/FRXX` — level to boot into (useful for testing custom levels)
- `ShowMenuOnBoot`, `NoCutscenes`, `SkippableCutscenes`
- `EnableDebugControls = 1`, `AllowMasterCheats = 1`, `NoDamage = 0`
- `HostIP = 0`, `HostPort = 1000`, `IsServer = 0`, `EnableNetworkPlay = 0`, `NetworkLatency = 1`
- `[Level Progression]`, `[Debug Portals]`, `[ScenePlayerMapping]` sections
No external `fguy.ini` / `boot.ini` exists in the install today.

The install also has `automenu.ini` / `autotweaks.ini`, the save state of an in-game developer
"tweak" menu (e.g. `Player|Unlock All Costumes = true`).

## How config loading really works (confirmed 2026-10-08)
- Init code at `0x9a84ca` builds `"./fguy.ini"` and calls the loader at `0x99d3d0`.
- Loader first calls `FindResourceW(NULL, 101, L"INI")`. If found, it hashes the resource and compares
  against `0xee940f71`; on mismatch it calls `_exit(1)` (anti-tamper). Then it parses the built-in text.
- If the resource is **not found**, it opens `./fguy.ini` from disk instead (the developers' path).
- Our loader hides the resource and redirects `./fguy.ini` to `FGUYMod\<name>.ini`. **Confirmed working:**
  `ShowMenuOnBoot = 0` boots straight into FRXX.
- Key/value readers: `0x4eae30` = get int(ini, key, default), `0x4eaea0` = get string. The big settings
  parser is the function around `0x99d3a0`–`0x99db3c`.
- Network-related keys parsed (and where they're stored):
  | Key | Default | Stored at | Notes |
  |---|---|---|---|
  | `RealVine` | 0 | byte `0xbad6a5` | unknown (dev tool?) |
  | `HostIP` | `10.6.7.244` | char[32] `0xbad8ac` | a Heavy Iron office LAN address |
  | `HostDomain` | "" | char[64] `0xbad8cc` | |
  | `HostPort` | 5000 | word `0xbad90c` | |
  | `EnableServerSearch` | 0 | byte `0xbad90e` | |
  | `NetworkSessionGroup` | `PUBLISHER` | char[32] `0xbad90f` | |
  | `EnableNetworkSessions` | 0 | byte `0xbad92f` | |
  | `EnableNetworkPlay` | 0 | passed to `0x97f510` | turns the network module on |
  | `IsServer` | 0 | **read but ignored** | |
  | `NetworkLatency` | 1 | passed to `0x4dc240` | |

## Network play test results (2026-10-08)
- **Correction (2026-10-09):** the network module starts on *every* launch, even with
  `EnableNetworkPlay = 0` — the first runs just didn't log sockets yet. So `EnableNetworkPlay` controls
  something else (still unknown).
- The game opens **TCP listen on 0.0.0.0:7534** and sends a **25-byte UDP broadcast
  to 255.255.255.255:7533 once per second** (LAN "I'm here" beacon). Game keeps running normally.
- Beacon bytes: `19 00 00 00 | 0a 0b 00 00 | 00 | 6e 1d (=7534) | 00 ... 01 ...` — length, message id,
  then the TCP port to connect to.
- The ports come from the network system constructor at `0x747f70` (`+0x9c = 7534`, `+0xa0 = 7533`).
- Every copy behaves the same (peer-to-peer; `IsServer` does nothing). A second copy on the same PC
  fails to bind 7534. Nobody bound UDP 7533 in our tests, so the "listen for beacons" side is started
  elsewhere (possibly from a menu flow). `EnableServerSearch`/`EnableNetworkSessions` = 1 changed nothing visible.
- Running two copies on one PC needs windowed mode (otherwise the second fullscreen copy fails to
  create its D3D9 device and crashes at `0x4876e9`, a NULL device pointer `[0xaf345c]`).

## Window, focus and input (2026-10-09)
- Main window class `GameWindowClass`, title `FGUY`; there's also a hidden `STATIC` "InputControlWindow".
- The game **pauses when its window loses focus**; the loader's `RunInBackground` hides focus loss
  (swallows `WM_ACTIVATEAPP(false)`, `WM_ACTIVATE(WA_INACTIVE)`, `WM_KILLFOCUS`).
- Keyboard input is not read from window messages (posted `WM_KEYDOWN` is ignored).
- Controllers: XInput (`xinput1_3` ordinal 2 = `XInputGetState`) is polled for all 4 pads from a tight
  loop (~6000 polls/s). Menus auto-repeat held d-pad buttons, so injected taps must be short (~40 ms).
  Virtual pad injection works in menus.
- Registry `IsKeyboardMouseInput` (1-byte REG_BINARY) picks keyboard/mouse vs controller mode. In
  KB/M mode the title screen ignores controller buttons. Loader `[Debug] PadMode=1` reports 0.
- **Local multiplayer join screen accepts virtual pads:** with virtual pad 1 plugged in, pressing A on it
  joined as player 2. This is the basis for "host + remote input" online play.
- **Crash:** skipping the intro logo movie (A press during it) crashed in `XAudio2_7.dll`
  (offset `0x3da72`). Skipping the attract-mode trailer was fine. To investigate (Bink → XAudio2 teardown).

## Menu flow (controller mode, 2026-10-09)
Intro movies → attract trailer (if idle) → title "press start" → **Select Save** (3 slots; X = delete!,
B = "quit to title?") → **Main Menu**: Story Mode, Multiplayer, Challenge Mode, Options, Store,
Bonus Materials, Exit Game.
- **Multiplayer**: Deathmatch, Multiverse Madness, Capture the Greased-Up Deaf Guy, Infiltration!
  (level suffixes DM / HD / CF / IN). No network/online entry in the vanilla menu.
- **Match setup** (Deathmatch): Map, Time Limit, Winning Score, Teams/FFA, Friendly Fire → Next.
- **Join screen**: 4 panels, "Press A to join" per controller, then Select Team (Good/Evil).
- The game autosaves the selected slot on menu transitions ("Saving..." icon).

## Menus: Scaleform GFx 3.x (2026-10-09)
- Menus are Scaleform GFX movies (Flash 10 / **ActionScript 2**), uncompressed inside the `.ho` packages.
  `main_menu.gfx` (290,635 bytes + 1 pad byte) is in `Global_Scenes/MNUS.ho`. Other movies seen:
  pause_menu, stats_screen, objective_screen, Dialog, trcMessageBox, start_screen, credits, icons, cursors…
- `Scaleform::FileOpener::OpenFile(url, flags, mode)` = vtable `0x9fe670` slot 1 (`0x4421d0`); returns a
  `Scaleform::GSFMemoryFile` (vtable `0x9fe61c`; +0x08 path, +0x0C data, +0x10 pos, +0x14 length).
- Menu → C++ calls: `ExternalInterface.call("CATEGORY_Name", ...)` →
  `Scaleform::ExternalInterfaceHandler::Callback(movie, name, args, argc)` = vtable `0x9fe56c` slot 1
  (`0x454190`). Return values go through the movie's vtable slot 50 (`SetExternalInterfaceRetVal`).
  `GFxValue` = { objectInterface*, type (2 bool, 3 number, 4 string; 0x40 managed), 8-byte value }.
- Categories seen: GRFX (graphics), OPTN, MENU, SCEN, SAVE, CTRL, PROF, SYST, LOKR, STOR, MULT, PLYR,
  FGUY, and **NETW** (`NETW_IsServer`, `NETW_SetActiveMenu`, `NETW_OnScreenEntered` are called by the
  main menu — the network layer is wired into the menus).
- Tools: `tools/re/swf.py` (tags), `tools/re/as2.py` (AS2 disassembler), `tools/ui/gfxpatch.py`
  (assembler + patch writer), `tools/ui/make_menu_patch.py` (the Borderless menu patch).
- Options > Screen script: `OnOptionsScreenEnter`, `GetDisplayModeValue`, `OnDisplayModeValueChanged`,
  `OnOptionsScreenNextButtonPressed` → `GRFX_ChangeResolution(width, height, windowed)`.

## Window procedure and screen size
- Game window proc `0x483920` handles only WM_DESTROY, WM_ACTIVATE, WM_CLOSE, **WM_WINDOWPOSCHANGED**.
  On WINDOWPOSCHANGED in windowed mode it sets the screen size globals (`0xb06360` width,
  `0xb0635c` height, `0xb06354` windowed flag) from the window's client size — even for SWP_NOSIZE.
- `0x419680(windowed, x, y, w, h)` applies a resolution (called from `0x4cac63`); the Options page has a
  "keep this resolution?" timer that reverts after ~16 s.
- DirectInput keyboard cooperative level is `0x16` (includes DISCL_NOWINKEY → Windows key blocked).
- Intermittent startup crash at `+0x876e9` (NULL D3D device). Loader now retries `CreateDevice`.

## Leftover netcode verdict (2026-10-09)
Not a usable online mode. `IO::NetworkModule`'s vtable is mostly `ret 4` stubs (`0x7af040`); the only
gameplay message (`FGUYPlayerLocalAuthorityUpdateMessage`, built in dead function `0x647d50`) is never
sent. The TCP 7534 / UDP 7533 beacon is most likely the developers' editor link (GoodEditor,
`EnableRealTimeUpdate`). Network ini keys sit with other dev-only options (watermark, soak tests).

## Players
- Class `zFamilyGuyPlayer`, vtable `0xa1af14` (167 slots); base/sibling `zNetCommonPlayer` (`0xa8e49c`).
- `+0xdf8` player index, `+0x38` → transform, position at transform `+0x30` (x, y, z floats).
- Story level FRXX has 3 player objects (index 0 Stewie, 1 Brian AI partner, 2 unknown/inactive).

## Determinism / lockstep findings
- Random: `srand` seeded from QPC at startup (`0x4ca150`); more RNGs: Mersenne Twister (`0x4f6790`,
  `0x49ed60`), LCGs (`0x41f260`, `0x4f67e0`, `0x6fab40`, `0x8601b0`, `0x8c3050`).
- Time: QPC used 108×, GetTickCount 12×, timeGetTime 4×; `LockFramerate = 0` (variable dt).
- Threads (2026-10-10): the MAIN thread (the one that loads dinput8) runs game logic. Each tick:
  wait for the render thread's go-ahead, poll XInput pads 0..18 itself (`0x4cc4a0` → `0x4b5670`;
  keyboard/mouse `0x450930`), update (handing jobs to `_beginthreadex` workers, start `0x74B7E0`),
  then hand the frame to the render thread, which draws/presents while main starts the next tick.
  DirectInput joystick loop `0x47b080` (Sleep 15). The game pauses itself at level start.
- Lockstep design that works: step at main's first pad poll of a tick; fixed clock on main =
  base + tick/60 s; fixed srand; synchronous overlapped reads; before each tick wait until all other
  threads (except render) are parked in a hooked wait/sleep/socket call (exited threads skipped).
  Result: 0 desyncs over 4200+ ticks at 60 ticks/s, two copies, scripted movement; hashes are even
  identical between separate runs.
- Leftover dev link thread blocks in `accept()` on TCP 7534 forever (second copy fails bind and exits).
- Loading screen (Stewie's weapon closet): main waits in `0x4c9310` (MsgWaitForMultipleObjects on
  `[0xaf38bc]`) while the render thread (`0x47a110` loop) shows it and polls XInput hard; it waits for
  an A press once the level is loaded (~3.5 s), otherwise ~3.5 minutes. Netplay forwards a synced skip.

## Display settings
Registry `HKCU\Software\Heavy Iron Studios Inc.\Family Guy\Settings`: `ScreenWidth`, `ScreenHeight`,
`ScreenWindowed`, `ScreenMonitor` (4-byte REG_BINARY), `Language`, `IsKeyboardMouseInput`, `KeyConfig`,
`MainWindowPlacement`. The loader can override these per run (see README).

## File loading
All game data is opened via `CreateFileW` with relative paths like `./FGUY/Levels/AMDM.ho`, so the
loader can redirect individual files to modded copies later. The game also probes `./boot.ini` (not present).

## Data files (`FGUY\`)
- `.ho` — main asset/level packages (69 files, ~2.5 GB). Magic `HEL\x1a`, version 1, build timestamp
  at 0x10, data begins ~0x400. Contents are **uncompressed** (readable strings, Havok `hk_2010.1.0-r1`
  physics blobs, AI behavior-tree names, tool tag `GoodEditor:PackView.GEPlugin`).
- `.fsb` / `.fev` — FMOD sound banks / event files (standard, existing tools can open them).
- `FMV\*.BIK` — Bink videos.

### Level naming
`<2-letter world><mode>`. Worlds: AM, AP, BP, CH, CT, FR, GC, MC, PI, SS, TF.
Suffixes: `XX` = story level, `DM` / `CF` / `HD` / `IN` = multiplayer modes (likely Deathmatch,
Capture the Flag, King of the Hill/"Hold", and Infection, to confirm), `C1`/`C2`/`C3` = challenges,
`MP` = multiplayer hub(?). `Levels_Loading\LDxx` = loading screens.
Player packages: `Global_Scenes\Players\PLYS/PLYW/PLYN/PLYX`.

## Menu 3D character, view rectangles, controller keys (2026-10-10)
- The Store's 3D character is menu player 2 in the MNUS scene. Store entry: `PLYR_SetJoinEnabled(false)`,
  `DropOtherPlayers()`, `MENU_EnableStore`, `GAME_Run MNUS_Script_Camera_StoreEnter_ref`. Model:
  `PLYR_SetCharacter(2, GetCharacterIndex(name))`, then `PLYR_ApplyCostume(2, CostumeIndex)`. Exit:
  `PLYR_ResetRotation(2)`, `PLYR_Refresh(0/1)`, `MENU_ExitStoreCam` (in MNUS), `MENU_DisableStore`,
  `GAME_Run MNUS_Script_Camera_StoreExit_ref`. Full screen, that camera shows the model at ~65% across,
  6%..87% down.
- Render camera rectangle (0x403480, camera +0x34..+0x40) is **left, top, right, bottom** as fractions
  of the screen (not x, y, w, h). Outside it the screen is cleared black. The online lobby uses this to
  draw the menu scene inside a panel (FGUYMOD_MenuView).
- Controller buttons reach menu key handlers as: A = 65 then 13, Start = 36 then 32, **Y = 89** (112 is
  only the keyboard's F1), X = 88, LB = 76, RB = 82, Back = 35.
- `screenshot_<level>.tif` level shots have a TV bezel painted in; zoom ~1.6x to show only the picture.
- `getPixel` on `BitmapData.loadBitmap` pictures returns 0 (black), so it can't sample their colours.
- The level HUD's layout call (our `FGUYMOD_HudSplitMode`, make_hud_patch.py) is the first sign a level
  is running (MP and story); the HUD icon path builder already runs on the MP join screen.

## Player state for sync checks; cameras and lockstep (2026-10-11)
- The game's live player list: `[0xbad988]` = up to 8 player pointers, count at +0x20 (includes placeholder
  players). Position: `[player+0x38]+0x30` (3 floats). Camera / aim point: `[player+0xf88]+0xcc`; only
  meaningful for players with a viewport (`player+0xdfc` = 0..3), placeholders point at leftover memory.
  Found with tools/re/memwatch.py (snapshot, move with tools/kbm_send.ps1, diff).
- **Switching a viewport's render cameras off (0x476390) stops that player's camera / aim updating**, so
  every PC simulated the other players differently (real desync as soon as they turn or strafe). Full screen
  online now keeps every camera on and draws the others into a 1-pixel rect (no fps cost seen).
- The earlier "0 desyncs" results hashed a spawn-fixed value and didn't prove sync; with position + aim
  hashed, MP and story co-op stay in sync with both players moving (2026-10-11).
- Windows + Python asyncio: an ICMP port-unreachable on the UDP relay socket makes the proactor loop close
  the socket silently (logged only in debug mode). Fixed with SIO_UDP_CONNRESET off + reopening.
- In the menus the controller's Y arrives as key 89; DirectInput's mouse takes over raw mouse input when it
  starts (re-register raw input after it).

## Keyboard / mouse internals (2026-10-11)
- Controllers are slots ConsolePad0..19 (IO::ConsolePadDevice, vtable 0x9fd18c; slot +8 = device, +0x18 =
  index). Slot 0 = the keyboard / mouse device (type 8 at device+8), slots 1.. = XInput pads 0... A player's
  input object is `[player+0x101c]` (zPlayerInputHuman, vtable 0xa1a29c); its device is `input+0x30`,
  controller slot `input+0x34`. Devices are 0x6dc bytes; the state is at device+8.
- Slot Update 0x49bb90: copies the state's current half to its previous half (keys +0x47c -> +0x57c, cursor,
  wheel, buttons +0x460 -> +0x478), then polls via 0x4cc4a0 (type 8 -> 0x450930, pads -> 0x4b5670).
- Keyboard / mouse poll 0x450930(state, index): raw poll 0x4b6ad0 (DirectInput mouse -> globals: deltas
  0xaefae4/e0/dc, sums 0xaefaf4/f0/ec, buttons 0xaefae8, cursor 0xaefad8/d4 from GetCursorPos), keys copied
  from the game window's key table `[0xad39e8]+0xc2` (256 bytes, Windows key codes, 0x80 = down), mouse ->
  analog +0x30/+0x34 (clamped +-60), WASD move vector +0x14/+0x18.
- Player input queries (zPlayerInputHuman vtable [21..23]) check the device type: 8 -> KeyConfig table at
  0xadc290/0xadc294 (action*8: {context, key}), keys < 0x100 Windows codes, 0x100.. mouse buttons / wheel.
- Global keyboard mode 0xacc4cd (from the registry's IsKeyboardMouseInput) is checked by gameplay code too.
  In KBM mode a level that starts in KBM mode stops at "CLICK ANYWHERE TO START" before its HUD.
- Online: every player's raw KBM state travels in their inputs; each keyboard player gets their own device fed
  by the game's poll; mode 0xacc4cd turns on once every player's HUD is up (told through the inputs).

## Menu input, volumes, split cover (2026-10-11)
- **Menus read the keyboard from a controller device**, not from window messages: each movie's input update
  (`0x5a0280`, thiscall, 1 arg) builds per-controller menu buttons from the slot devices (`0x4254e0(index)`:
  table `[0xaefacc]`, count `[0xaf35ec]`, enabled `+0x6d0`). In keyboard mode, for the keyboard's controller
  (`[0xbad7f0]`) it reads the device's key array by key code at `device+0x484` (0x80 = down): Esc, Enter,
  arrows. Allowed controllers: the movie's mask (`[movie+0x104]`, often `[0xade6a8]`). `0x46fb00` turns
  buttons into GFx key events with the table at `0xacee88` (Start 36, Back 35, arrows, LB L, A 65...).
  The game fills the key array from the window's key table (`[0xad39e8]+0xc2`), which is empty whenever the
  window isn't in front - online the mod writes the synced keys into the array before the update.
- The window procedure (`0x483920`) only sets the key table on WM_KEYDOWN; WM_KEYUP and mouse button ups /
  wheel go to its listeners (`0x4c9180(code)`; mouse ups via `0x42f530`, which also shows the cursor).
- Paused games don't Present and their netplay ticks run unthrottled (~280/s).
- **Volumes**: sound manager at `0xb59ea8`: `+0x3ac` music, `+0x3b0` effects, `+0x3b4` cinematic, `+0x3b8`
  dialog (0..1). `0x665d50` applies them to the FMOD categories (`0x64d9f0(index, volume)`), with a 0.6
  factor (`0x9f3c90`). Movies: `0x4ea2d0` = cinematic x 32768 x `[0xadaa58]` -> `BinkSetVolume`. The menu
  sliders step by 0.1 per press (`OnSliderHandleInput`), 0.01 while held.
- **Split-screen cover** (`cover.gfx` in BULK.ho) lays itself out from `SYST_GetNumViewports` and
  `OPTN_GetSplitScreenOrientation`; side by side (`Viewports2V`) draws a line down the middle.

## Client-server research: health, damage, moving a player (2026-10-17)
- **Health**: `[player+0x105c]+0x50` = health, `+0x54` = max health (floats; 200 / 200 in Deathmatch). Player vtable
  `+0x16c` GetHealth, `+0x174` GetMaxHealth, `+0x178` SetHealth(float), `+0xbc` IsDefeated (health at 0, or
  state 7). Writing the health directly works (the screen goes red when low) but the HUD bar only updates through
  the damage path (`0x99a940(health, max)`).
- **Damage**: player vtable slot 58 (`+0xe8`) = `0x6901a0` TakeDamage(DamageInfo*): invincibility checks, hit
  reactions, knockback, the damaged sound, HUD, defeat; it calls `0x9855a0` (apply: computes the amount with
  `0x9812a0`, then SetHealth). DamageInfo: `+0x00` source entity (players have type `0x6f` at entity `+0x20`),
  `+0x1c` amount, `+0x20` damage type, `+0x28` flags, `+0x30` direction (3 floats), `+0x60`/`+0x64` more.
- **Moving a player**: writing the position at `[player+0x38]+0x30` teleports it for good (the Havok character
  proxy follows the transform; collision pushes it out of walls). Walking afterwards continues from there.
- The unused `FGUYPlayerLocalAuthorityUpdateMessage` (built in `0x647d50`) shows what the developers meant to send
  for a player whose own PC is in charge of it: player index (`+0xdf8`), `+0x1a34` (a move state: 0 idle, 2
  walking), a one-shot flag (`+0x10b3c`), position, facing (atan2 of `[player+0x5c]+0x88`), `[+0x2240]+0x18`,
  whether `+0xeb70` is 2 or 3, and the aim (vtable `+0x20c`: `+0x14d4` vector unless `+0x14d0` = 2).
- Research tools: `tools/re/statewatch.py` (whole player objects: snap / diff / find), `tools/re/pipe.ps1`
  (send a pipe command), new pipe commands `hold` (sticks, triggers, buttons for a time) and `poke` / `pokef`.
  Research copies run from `A:\Games\FGUY-Research` (hard links and junctions to the game, its own
  dinput8.dll and FGUYMod folder), never from the real game folder: pass `-GameDir` to the test scripts.
- **Shots seen by the damage spy** (`[Debug] DamageSpy=1` / env `FGUYMOD_DAMAGESPY=1`, research.cpp): Lois's gun in
  Deathmatch = TakeDamage per bullet, source = the shooting player (type `0x6f`), amount 15, damage type 28, flags
  `0x400440`, direction set, `+0x60` = 1. 200 health = 14 bullets; the kill gave the shooter 350 points. A defeated
  player respawns at a random spawn point. An instant kill arrived as amount 1000, type 1, no source, flags 0,
  right after a test teleport put player 0 into player 1 (the game's own "stuck / out of the world" kill):
  corrections must never drop a player inside another one.
- **Cameras**: each player carries its camera inside its object (a camera at `+0xf0c0`, updated by `0x588c40`, the
  zCamDriver / zCamThirdPerson update): the yaw at `+0xf32c` (radians) and `+0xed64` (degrees), pitch `+0xed5c`,
  and the matrix at `[player+0xf88]+0x9c` are all outputs, recomputed every frame from the camera's eye and target
  points (`0x4ac8e0`, atan2 / asin) - writing them does nothing. **The camera's own state** is the orbit block at
  `player+0xf3a8` (camera `+0xf148` = zFGUYCamThirdPerson, block at camera `+0x260`): pivot (3 floats), **yaw**
  `+0xf3b4` and **pitch** `+0xf3b8` (radians), distance `+0xf3bc`, eye (3 floats) `+0xf3c0`. The stick adds to the
  yaw every frame (`0x580ab0`); writing yaw / pitch turns the camera for good, and the player's shots follow it
  (aim = the viewport's view through the screen centre, `0x6568c0`): tested, a shooter turned only by writing its
  yaw hit its target. View direction on the ground = (-sin yaw, -cos yaw).
- **Teleporting**: writing only the transform position makes the character *slide* there (the Havok character
  proxy sweeps to it and stops at obstacles - that's what happened at the crates). A true teleport writes the proxy's
  shape too: `[player+0x104c]` = hkpCharacterProxy, `[proxy+0x60]` = its shape phantom (hkpCachingShapePhantom),
  position at phantom `+0xe0` (a little above the feet: keep the difference). Writing both together (pipe command
  `teleport <player> x y z`) moved a player across the map through the crates, and it landed on the ground. Give a
  real height: a point under the ground makes the game put the player back. The transform pointer can change
  (respawn): read it fresh. The respawn itself is `0x69d5a0` (spawn point chosen through `0x69bfb0`).
- **Final damage differs from the hit's amount**: point blank, Lois's bullets said 15 but took 45 each (the apply
  step scales it, `0x9812a0`). The shooter's PC must send the health actually lost.
- **Melee** (B): amount 10, type 30, flags `0x401240`, source = the player. **Grenade** (LT): 150, type 28, flags
  `0x20348`, direction = the blast vector; it hit the thrower too (self damage) - source = the thrower for both.
  LB gave a 0-damage hit, type 1, flags `0x40`. Every player-caused hit names the player as its source.
- Facing: the transform at `[player+0x38]` is a 4x4 matrix, rows right / up / forward / position.

## Pickups, and the free-running match (2026-10-17)
- Deathmatch maps have pickups (Frat Row's level file: PU_Ammo_*, PU_HealthFull_*, FRDM_SupplyCrate1-3, FRDM_Pickup1-5
  for the MP power-ups: DoomBear, GiantChicken, Ipecac, JoeTurret, StewieTurret, TubeMan). Ammo and health are
  `zFloatingCollectible` objects (vtable `0xa8deb4`, entity type `0x8d`): `+0x18` = the pickup's *spot* id (the same
  on every PC), `+0x68` its kind (asset), byte `+0xa3` = out to be taken, `+0x70` = the player taking it, `+0x74` =
  flying to them. A spot has one object per kind it can show (some spots 6 kinds, most one); byte `+0xa1` = the kind
  switched on there. It's picked once, as the level loads, and a taken pickup comes back as the same kind (~110 s
  later) - the pickup's own timer, `0x9ba730` resets it, nothing picks again. Online the level loads in lockstep, so the
  kinds are the same on every PC: checked spot by spot, host vs guest and host vs a game that rejoined (it loads the
  level alone - same kinds). Active = `+0xa1` and `+0xa2` set, `+0x75` (taken) clear (`0x9719a0`); `0x9bcfb0` shows /
  hides it by that. A pickup is only taken if the player needs it (full health / ammo: nothing happens); health
  pickups give 50. Kind ids seen: `0x42`, `0x103`, `0x1b9`, `0x2363` (single-kind spots), `0x80`, `0x83`, `0x89`,
  `0x240e`, `0x2412`, `0x3630` (the 6-kind spots).
- `0x9beee0` (thiscall: the pickup, the player) starts the taking (sets `+0x70` / `+0x74`, sends the player event
  `0x1d05330b`, which gives the item); the pickups' update is vtable method `0x9c0f40` (`ret 4`).
- The free-running match (netplay.cpp, MATCH SETTINGS > NETCODE: New): tested against the scripted bot through a local
  lobby server with 150 ms each way, +-20..30 ms jitter, 2% loss - remote players ~0.1-0.15 units off on average,
  hits identical on both PCs, kills, respawns (one teleport), match end, rematch, quitting, rejoining (back in ~14 s),
  pickups (spot by spot). Story co-op stays on the classic netcode for now.

## Power-ups (2026-10-18)
- The power-ups are the pickup spots with six kinds (`powerups.py`): Frat Row has five (`0x4ba9`, `0x4bb0`..`0x4bb3`
  = FRDM_Pickup1-5), each with kinds `0x80`, `0x83`, `0x89`, `0x240e`, `0x2412`, `0x3630`. The kind switched on at each
  was the same in every match (3630, 89, 3630, 2412, 240e). They come back 60 s after being taken. Taking them is
  already shared (they're `zFloatingCollectible`s, see above). `0x2412` = the Giant Chicken (HUD icon under the
  portrait, "x n" = how many held).
- Using one: D-pad left / right picks the held power-up (the HUD icon changes), D-pad down uses it (Peter makes a
  summoning gesture). On keyboard: the weapon wheel (`` ` ``), then Ctrl = the inventory page (8 slots), by W/A/S/D.
  The first D-pad down after taking one sometimes did nothing; after D-pad up, down worked.
- Using the chicken creates an AI character (`vtcount.py` before / after: behavior tree nodes `zNPCBT...`,
  `zNPCRegulatorRuleGroup`, `hkpGskBaseAgent`, ~45 new classes), gone again about a minute later. "IsGiantChicken"
  (`0x5a839e`) is a UI condition; `0x698a10` registers the names Ipecac / GiantChicken / TubeMan with a number each.
- **In the free-running match a power-up used on one PC shows only there**: the other PC gave the pickup to its copy of
  the player, and the D-pad press reaches it (remote inputs keep every button that went down), but its copy never
  used it - none of the new classes appeared there. So using one is local-player only (the HUD's inventory,
  `HudElements::InventoryPickup`, vtable `0x9f5524`, probably drives it). To share them: find the call that uses the
  item for a player, send it ('P': player, kind), make the same call for their character on the other PCs - and the
  AI character it makes needs syncing like the other modes' enemies (where it is, who it attacks, its damage).
- Looking for the code that uses one (2026-10-18, not found yet):
  - The pad's buttons in the game's bits (`0x4b5670` -> table `0x9fc4a0`): Start 1, Back 2, D-pad left 4, right 8,
    up 0x10, down 0x20, then the rest; A B X Y as XInput's 0x1000.. 0x8000. So D-pad down = action 14 (ctx 2), but
    nothing asks the player's input for action 14 through its queries (zPlayerInputHuman slots 21..23, the input spy:
    FGUYMOD_INPUTSPY=<action> or 999 for all) - neither on the PC that used it nor elsewhere.
  - The player's weapons / inventory input: `0x69b5f0` (each frame). Actions 40 / 41 (D-pad right / left) cycle the
    selected slot, player `+0xef70` (`0x698190` next, `0x698250` back, both `0x697ff0(item)` to select it). Consumables
    are weapon ids (`0x698a10` registers GetConsumablesWeaponIDs: Ipecac, GiantChicken, TubeMan, DoomBear, Joe,
    SentryTurret, Health), next to ThrowBegin.
  - Player method counts around the press (FGUYMOD_PLAYERSPY=1, pipe "spymark <label>"): only slot 24 (a getter) more,
    on both PCs alike. Using it on cue was unreliable in the tests: often only Peter's gesture and nothing more, the
    count staying (twice the chicken came - facing open ground; facing a wall or a gate it didn't).
  - Ghidra (headless, the analyzed project): `tools/re/ghidra/decompile.ps1 <out> <hex> | callers:<hex>`.
- **LB uses the selected power-up** (pad 0x40, action 22 / 38, keyboard Q) - D-pad down didn't (six tries, the count
  stayed; it plays a gesture). How many are held: player `+0x2aac` for the chicken (`countscan.py`), the selected slot
  `+0xef70` - the same on both PCs (the pickup replay gives it to their character, the D-pad presses select alike).
  In the free-running match LB on the host took the count 2 -> 1 on **both** PCs, and both show what it threw (a red
  chicken bucket on the ground, same place). So using one already reaches the others through the relayed press. What
  the bucket does next (and the big yellow thing seen twice after D-pad down) isn't known yet.
- **All six tested in the free-running match** (host on the virtual pad, the bot on the other team - `test_bot.ps1
  -BotTeam 1` - held next to the throw): `0x2412` Giant Chicken (a giant chicken fights the enemy), `0x89` Boom Bear
  (explodes), `0x240e` Joe Swanson (Joe in his wheelchair shoots), `0x3630` Rupert-In-The-Box (a turret, darts),
  `0x80` Wacky Waving Inflatable Arm-Flailing Tube Man (a decoy, no damage), `0x83` Ipecac (a burning vomit plume).
  Frat Row only has the first four; the other two were tested by switching a spot's kind on both PCs (+0xa0..a3 state
  bytes, `switchkind.py`, research). Each appears on both PCs (each PC its own copy, from the relayed LB), and their
  damage is "no source" (player -1): decided on the victim's PC, the same health on both (checked hit by hit).
- **Crash found and fixed**: a no-source hit that kills, replayed on another PC, crashed there (`0x647563`, the death
  fall's direction from `[source+0x38]`, source null - Rupert's darts carry a body part). Replays of no-source hits
  now go without the body part (as a fall), tested: Rupert and the chicken killing the bot, no crash.
- Seen on the way (for the polish): after the chicken's kill the host's copy of the bot was up again 5 s before the
  bot's own PC had it respawn.

## Supply crates (2026-10-18)
- A supply crate is a store portal: `zInteraction` (vtable `0xa8d6fc`, `+0x18` id, `+0x28` -> its menu,
  "=MNU_GLOBAL_SUPPLYCRATE") with a `zTrigger`, scripts, a map icon and a `zPlatform` (`0xa19844`, where it is) of the
  same id (`crates.py`). Frat Row: `0x4e30` (35.9 7.4 112.0), `0x4d39` (118.0 8.0 88.5), `0x5155` (40.7 8.4 201.0).
  Some maps also have `=GLOBAL_AMMO_DEPOT` interactions (ammo, no menu involved).
- "PRESS X TO ACCESS STORE" opens the HUD's CharacterCustomize screen as the in-match weapon select (the wheel of the
  multiplayer weapons; left stick picks, X buys, B or START leaves: `FGUY_WeaponSelectDone`). Buying: the HUD calls
  `STOR_SubtractCurrency(player, cost)` then `STOR_SetMultiplayerWeaponBought(player, weapon)` (the game's player
  number; weapon 21 = SVG, 1000). Money = player `+0xbc+0x224` (kills pay it).
- A bought weapon is used from the next life: the death menu's CHANGE WEAPONS (Y) is the same wheel, left stick picks,
  START respawns with it (B goes back). In the match a short Y swaps between the two weapons; a held Y opens the wheel
  (LB / RB its inventory page). Pad bindings: `0xadbac8 + action*8` {context, buttons - XInput's bits for A B X Y},
  the keyboard's `0xadc290`; action 28 (the weapon wheel) = Y / `` ` ``.
- Online the HUD runs every player's screens on every PC: someone's store opens on the others' PCs too (not shown -
  their HUD has no viewport there), driven by their relayed presses. In the free-running match money wasn't shared,
  so it refused there - now money comes with the state ('X'), what their store would buy on another PC is dropped,
  and the purchase itself goes out ('W': weapon) and is marked bought for them with the same menu call (ui.cpp).
- Online the loader gave START to its pause menu always, so a controller couldn't leave CHANGE WEAPONS (any netcode).
  The HUD now tells it when that screen is up (`FGUYMOD_HudTakesStart`): START goes to the game. Another PC's START
  only reaches their character here while it's down (else, at a different point here, it paused this game).
- Tested (free-running, host on the virtual pad): buy (host 5000 -> 4000, the guest the same), die, CHANGE WEAPONS,
  SVG, START, respawn on both PCs, tap Y - the SVG in hand on both.
- Tools (power-ups and crates): `vtcount.py` (every object in the heap by class, snapshot / compare), `vtpick.py` (the
  count of given classes), `vtname.py` (a vtable's class name from RTTI), `countscan.py` (find a counter by how it
  changes, like a cheat finder), `crates.py` (a level's supply crates), `lookat.py` (a test copy's camera turned onto
  another player, for screenshots); the test pad's `hold` / `press` also know `ls` / `rs` (the sticks' buttons).
