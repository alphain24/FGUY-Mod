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
- **Joe's accuracy online = offline (measured 2026-10-22, 3-player DM, new netcode, a standing target):** Joe (kind
  0x163:188 once out) hits every ~0.2 s for 10, a full 200 down in ~4.5-5 s - the same whether the target's own PC is the
  host (its Joe the game's own AI, as offline) or a guest (its copy follows the host's). Every PC had the same hits; the
  second target (further away, same team as the first) was never shot - no "everyone at once". So the reported aimbot is
  the game's own Joe. He only attacks the other team (same team: he idles). Test tools (scratchpad): kindon.py (a spot
  shows a kind), hplog.py (every PC's view of every player's health).

## Scoreboard and clock (2026-10-18)
- The game fills the scoreboard with `UpdateMultiPlayerStats(team, player, name, score, bonus, kills, deaths)`
  (`0x6673b5`): the player's +0xe84 score, +0xe88 bonus (medals), +0xe20 + +0xe30 kills, +0xe2c deaths, +0xe90 name
  (`scores.py`). A kill is credited in `0x5d6450`: the hit's source -> `0x968940` (a summoned character, type 0x11f:
  its owner at +0x8e4) -> the player; +1 kill, + the kill's points. Medals (FIRST BLOOD: 100) go out in `0x9a4fa0`.
- Free-running, before: a power-up's kill counted only on the victim's PC (the others replayed it without a source):
  host 3 kills / 525 there, 6 / 1050 on the bot's PC. Now the deciding PC sends whose kill it is (the hit's `credit`)
  and the others replay it with that player as the source - then the same. A medal for a power-up's kill still comes
  only where its character made the kill, so the host's count is everyone's ('T', on change and every second).
  Tested: the bot's PC had 100 bonus more, took the host's.
- A kill's medals: the death's event (`0x99eec0`, by the damage type) calls `0x66eb90` (thiscall: the killer, the hit,
  the victim; it counts the killer's kills at +0xeb24 - the first: FIRST BLOOD, medal 0 - and queues medals with
  `0x97e760(medal, player)`). For a power-up's damage type it credits the source only when that's the character
  itself, so a replay with its owner as the source got no medal. Replays of a power-up's kill now call it for the
  owner (if the game didn't). Tested: the chicken's first kill - 100 bonus on both PCs.
- Others' aim (2026-10-18, `aimsmooth.py`: the owner turns with the stick held at full, 270 deg/s, the other PC's copy
  sampled every 4 ms; a scratch script timed quick stops). Through the local server with 100 ms each way +-20, 2% loss
  (ping 219 ms): before, the copy followed each state ('X', 30 a second) with easing - 43.7 deg behind while turning,
  its speed frame to frame 61% spread (the owner's own 33%); at a stop behind up to 32 deg, settled in ~0.2 s. Now
  it's aimed where they are by now (their turn speed from the last states, by their clock - `sentMs` in 'X' - times
  the state's age + a quarter of the ping + the easing's 1/20 s, at most 0.15 s): 14 deg behind, 52%; a stop swings
  past by up to ~20 deg, settled in ~0.23 s. All of the ping: 8 deg behind but more past at stops. Without ping: 1 deg.
  `FGUYMOD_TEST_NOAIMPREDICT=1`: as before (to compare).
- Respawns: a player who died and pressed A (once, mashed, or after X / B on the death menu) was back up on both PCs at
  the same moment (`hurt <player> <amount>` on the test pipe: a hit nobody caused, decided on their PC). The bot's 5 s
  gap earlier didn't come back with two test copies driven by the pipe (the bot presses on its own).
- 3 and 4 players, free-running (`test_online_match.ps1 -Players 3|4` with FGUYMOD_TEST_NETFREE=1 and
  FGUYMOD_TEST_CONF=DM,FR,1,0,1, through the server with 100 ms each way): everyone walking, each PC had each of the
  others 0.02-0.07 off on average (0.34 at most); a hit between two guests went to all three PCs alike; the scoreboards
  the same on every PC; the 5-minute match ended on all four within 85 ms. In the 4-player run the lockstep part before
  it lets go had sync check mismatches (tick 150 on one PC, 810-870 on another: the slowest to load, 4 games on one PC
  - the players 0.04 lower while falling into the level, 3 random numbers more); from tick 900 the free-running match
  corrects it. Checked again: 4 players with the classic netcode (no lag: 75 checks each; with the lag: ~207 each) and
  the new one (with and without the lag) - no mismatch at all. A one-off on an overloaded PC (4 games loading at once,
  right after other runs).
- The match clock: `0xbadcc0` (elapsed; the HUD's timer is the limit `0xbadcac` minus it). Both PCs within ~0.02 s
  after 4 minutes (both count real time from the same tick); it's compared with the host's, not corrected.

## Enemies / NPCs (2026-10-18, research - toward syncing them)
- `zNPCNetworking` (vtable `0x9f2960`, a cut network mode's) is stubs in the shipped game - its methods return at once.
- NPC classes: `zNPCEntity` (`0xa1a8dc`), `zNPCGeneric` (Multiverse Madness' enemies), `zNPCManager`, `zNPCSpawner` with
  `zNPCSpawnerLogicWave` / `Queue` / `Populate`, `zNPCLogic`, steering, perception, behavior trees (`zNPCBT...`).
  `npcs.py <pid> ...` lists a game's NPCs (class, id +0x18, entity type +0x20, kind +0x10 - 0x11f, owner +0x8e4).
- Multiverse Madness, free-running (research: `FGUYMOD_TEST_NETFREE=all` lets any mode run free), 20 s in: both PCs had
  14 enemies, all `zNPCGeneric` type 0x48, ids from one pool (0x1d3-0x1e1) - but the same id stood somewhere else on
  each PC, and the pools didn't hold quite the same ids. Each PC's spawner picks its own spawn points and order, so the
  host has to decide the spawns (and say what spawned where), then their states (where, facing, health, down).
- The class spy (`FGUYMOD_VTSPY=a1aafc:63` - zNPCGeneric's vtable, 63 methods - then pipe "spymark <label>": each
  method's calls, from how many objects and, for rare ones, from where) at a wave's start: methods 30, 31, 32, 55, 56
  once per enemy spawned (12, then 2: the wave's 14), 19 / 34 three times, 54 twice; later 50 (from `0x5e132f`) a few
  times a second while the bot shot them, 49 now and then.
- The spawn chain: `0x5f2ff0(?, type)` makes an enemy from its type (a pool object; `0x5f0aa0` sets it up: model,
  type data at +0x21c, its +0x218 template) -> slot 31 `0x5fb4a0` (its set-up: entity type 0x48, `rand()` for its
  looks), slot 55 `0x5fc310`; `0x60ce30` puts it in the world (its spawn transform at +0x22c, the spawn events, `rand()`
  again) - from `0x60cff0` and the pool's update `0x4c1600` (by state flags). Who picks the spawn point is next.
- **Spawning**: `0x60d080` (cdecl, a 0x6c-byte request from `0x5b14f0`: [2],[3] the context entity's id, [4],[5] the
  kind, [10] -> the team, [0xc..0xe] where, [0xf..0x11] rotation, [0x12] scale) returns the NPC or 0 (0 is safe - turning
  down `0x60ce30` instead crashed in the clean-up `0x5e0ec0`). Its one caller `0x60f490` is called by the spawners and
  by the player's animation events (`0x698430` case 6: the power-ups' summons, context = the player, team = player
  `+0x1018`). So with the host deciding the spawns ('E': the request), the power-up characters come from the host's
  game too.
- **Where an enemy is**: its physics body - a Havok `hkpRigidBody` (vtable `0xa5ae74`) at `[[npc+0x124] +8]` (`+0x124` =
  its `zNPCCombatCollisionRigidBodyListener`, `0x9f2308`): position at `+0x140` (with copies at `+0x120`, `+0x130`,
  `+0x1d0`, `+0x1e0`); the game copies it to the transform (`[npc+0x38]+0x30`, also `[npc+0x5c]+0x30`) each frame, so
  writing the transform alone does nothing. The steering (`zNPCSingleSteering`, `0x9f5644`, at `+0x418`) has it at
  `+0xc4`. Moving all of them by the same amount moves the enemy for good (`npcposcopies.py` found them). Pulling the
  height too, a little each frame, made them sink into the ground (2.5 under) - so only along the ground.
- **Health** `[npc+0x420] +0x60` (max `+0x64`). Damage to any entity: `0x99eec0` (**cdecl** victim, hit - declaring it
  stdcall broke the stack: 0xc0000409). The final damage is worked out a little differently on each PC (the same hit:
  10 on one, 20 on the other), so the PC of the player who hit sends the health it took.
- Synced (research switch `FGUYMOD_TEST_NPCSYNC=1`): spawns ('E'), state 10 a second ('G': where, health), players'
  hits ('V', with the health lost), down ('k' - 'K' is the catch-up record); the others follow the host's position each frame (its last + speed x
  age). Measured (Multiverse Madness, 220 ms ping, 2% loss): 0.25 off on average along the ground, 1.5-2.5 at most;
  health and who's up the same on both. Deathmatch: the Giant Chicken (900 health) thrown by either PC made by the
  host's game, the same on both, its damage the same on both, its kill and medals counted for its owner on both.
- **Facing**: the transform's forward row (`+0x20`; yaw = atan2(x, z)), turned each frame toward the steering's heading
  (`[npc+0x418]` `+0xbc` / `+0xc0`, 0..2 pi, forward `+0xa0`): writing the steering alone does nothing, the transform
  alone is turned back to the steering's. Turning both - all the way each frame (a part of the way left them 25 deg
  off: the AI here aims them its own way every frame) - 2-4 deg off on average at 220 ms ping (`npcfacing.py`).
- **Pool objects are used again**: the guest's object for one of the host's enemies later becomes another's - the old
  entry pointing at it has to go (else a hit was sent for the wrong enemy, and the state of a dead one moved it).
- **Multiverse Madness' waves**: one `zNPCSpawner` (`0xa1abfc`) with a `zNPCSpawnerLogicPopulate` (`0xa197f0`) - no
  wave object; the events "NPC Spawner: Wave Spawned / Defeated". With the host's game spawning, the waves come the
  same on both PCs. The match ends on each PC when it sees everyone down; the results screen (round, round time,
  kills, score, deaths) is filled once then (the clock written later doesn't change it). Seen there: the round time a
  second short on the guest - not the waves but a player hit counted twice: the victim's state came before the hit
  (its health already had it), and the hit took it again - a death a second early there. Fixed: a hit decided on the
  victim's own PC plays from the health it had there before it. After: the same results on both (0:46, 0.08 s apart).
- **On by default** in the free-running match (2026-10-18, after 4-player tests at 220 ms ping / 2% loss: Deathmatch -
  kills, deaths, scores, health the same on all four PCs every round, a power-up's character made by the host's game
  and shown on the three others within 0.1 s; Multiverse Madness - the same 14 enemies on all four, 0.2 off on average,
  facing 2-3 deg). `FGUYMOD_TEST_NPCSYNC=0` (research) leaves each PC its own. The classic netcode doesn't load it.
- Test tools: pipe `killnpcs` (the host: every enemy down), `vtfind.py` (a class's vtable by name), `objdiff.py` (a
  class's object in two games, field by field).

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

## The end of a match in the new netcode (2026-10-19)
- A real 3-player match (220-390 ms): 10 minutes in sync (others 0.07 off on average). At its end each PC's results
  menu was its own (RESTART MAP / NEXT MAP / MAIN MENU: `GAME_Reload`, `GAME_Portal` + the level, `GAME_Quit`) - a
  pick reloaded one PC: the host's restarted alone (its match clock went on from where it was), a guest's dropped to
  the title screen and its lobby (it had to rejoin, and its clock was then 626 s behind the host's).
- Now (free-running only): the host's pick, or everyone's rematch vote, goes to everyone (`'r'`), every PC goes
  back to its lobby (the pick becomes the menu's `GAME_Quit`; a vote's leave runs from the results movie's poll -
  the HUD doesn't run there), with a file for its lobby game (`FGUYMod
ematch_<lobby pid>.txt`), whose lobby then
  readies (guests) / takes the next map and starts once everyone is back (the host). Tested: vote, NEXT MAP (FR ->
  AM), MAIN MENU (back, no new match) - the next match running 16 s after the pick. The results menu follows the
  host's controller (a guest's A does nothing there; Y votes).
- A rejoined game takes the host's match clock (2+ s off).
- Message letters: `'R'` is "ready" (the session's first tick) and `'K'` the host's catch-up record - 0.8.15/0.8.16
  used `'K'` for an enemy down too, which swallowed the record: rejoining a classic-netcode match broke. Enemy down
  is `'k'` now, the rematch `'r'` (the server only reads `'a'`).

## Multiverse Madness in the new netcode (2026-10-19)
- Rounds: the level's script sends the game mode's events by name hash to its handler `0x6a06a0` (cdecl: ?, sender,
  event, args): "FGUY: MP: Increment Round Number" `0xf91ca714` (round `0xbadcb0` +1, an int), "Set Round Number"
  `0x93d11ddd`, "Notify Horde Round Ended" `0xa0cc1897` (`0xbadcb4` = the round: a player down waits for it -
  "Cannot respawn until round is over"), "Respawn All Players" `0x9def5921` (each player down: `0x69d5a0`). The hashes
  are in the name lookup `0x55e000`-`0x56a000` (cmp eax, hash; je -> mov eax, name). The script sends them from its
  spawner's waves - with the host's game spawning every enemy, the others' spawner has none: a player down there
  stayed down. Now the host's go out ('w') and the others' own are dropped (they do send some), the host's played.
  Tested: the guest down, the host's round ends at ~78 s, the guest back on both PCs; rounds 2 / 2 on both.
- No drops from enemies (kills pay money); pickups are the level's pool (171 on both PCs).
- In the lobby: NETCODE shows for Multiverse Madness too (lobby.as2js FGUYNewNetMode); the match's end (RETRY) takes
  everyone back and starts the next one (tested through the lobby, the bot as the guest).
- Test tools: pipe `god 0/1` (my player takes no hits - one copy); test_bot.ps1 reached the character screen instead
  of MATCH SETTINGS after picking a mode (fixed).

## Infiltration and Capture the Flag in the new netcode (2026-10-20, research)
- test_bot.ps1 `-ModeRights 2` is INFILTRATION (`IN`), 3 is Capture the Greased-Up Deaf Guy (`CF`).
- Infiltration's characters (its commanders and guards: 90 / 120 / 150 / 300 / 1200 health) come through the
  spawning `0x60d080` once the match runs free - so they're the host's already: 0.25-0.6 off on average on the guest.
  (npcstate.py pairs them by pool id - the same enemy has another id on each PC: it showed them far apart.)
- It doesn't use the game mode's event handler `0x6a06a0` (none in a 90 s match). Its goals are control points:
  `zControlPoint` (vtable `0xa8d86c`, 5 of them on Frat Row, ids `0x10f`-`0x113` the same on both PCs). Their events:
  "Control Point: Capture progress by Team 1 / 2" `0x40765633` / `0x40765634`, "... blocked" `0x701fc614`,
  "... inactive" `0x4d698403`, "Captured" `0xfa0fcd35`, "Captured by Team 1 / 2" `0x885ba00b` / `0x885ba00c`,
  "Destroy progress by Team 1 / 2" `0xbf45d8d2` / `0xbf45d8d3`, "Destroyed" `0xd5cb4d2e`, "Destroyed by Team 1 / 2"
  `0x4b780fe6` / `0x4b780fe7`, "Disabled" `0xb98571a7`, "Expired" `0xaeaeb416`, "Neutralized" `0xb7627868` - sent
  from `0x9a97c3`-`0x9a9899` (progress) and `0x9ac7ae`-`0x9acb6c` (state). Each PC works them out from where the
  players stand.
- The point's update `0x9ac6a0` (thiscall: point, frame time; `ret 4`): off (`+0x26` bit 0) or no volume (`+0x44`):
  nothing. Else for each player (`0xbad988`, count `+0x20`) not down: the volume test `0x61bd30` (thiscall: volume,
  `[player+0x38]+0x30` - true / false; volume `+0x84` the shape: 1 = sphere, centre `+0x70`), the team's bit
  (`0x96b8a0`) into who's there; who's there changed: start capturing / destroying (`0x9a97b0` / `0x9a9830`); the same:
  progress `+0x48` up (several of one team: faster, config `+0x1c`) to the config's `+0x18` - captured (state `+0x50`
  4, team `+0x4c`, the message `=MNU_GOOD_CAPTURE` / `=MNU_EVIL_CAPTURE`, the players' captures `+0xe60`), or destroyed
  (`+0x54` 4, `=MNU_..._DESTROY_KEG`, off); nobody: decaying (state 1, `+0x64` to config `+0x24`, then neutral 0).
  Captured: every `+0x2c` s the team's points (`0x673ba0`: team, 2, config `+0x28`); `+0x60` to config `+0x20` (60 s):
  expired (`=MNU_KEG_EXPIRED_BLUE/RED`, off). The point's config: `[point+0x40]`. Its id `+0x18`.
- Switching it on / off: the events Enable `0x2c9d0683` / Disable `0x71e42988` through the game's event dispatch
  `0x520c50` (cdecl: from, ?, to, event, args, only-if-on: sets / clears bit 0, passes the event to what's linked);
  `0x96b860` (thiscall: thing, event) sends a thing its own event. A timeline (`zTimeline` vtable `0xa22224`, its timed
  events `0x635f90`) switches the point on; which point: each PC its own pick (the host's game had `0x113` on, the
  guest's `0x10f`). With players of two teams it sometimes never came on (in the classic netcode too).
- In the new netcode (0.8.18): the host's game says who stands on each point and its state (`'q'`, 10 a second and on a
  change); on the others the volume test answers the host's who-stands-there while the point updates, their own
  Enable / Disable for points dropped and the host's sent, the host's progress and timers taken; a capture / destroy /
  expiry the host's had and this PC hasn't (counted on each), it finishes itself (progress / timer at the end) so its
  own game plays it out (messages, events, points); still apart for 1.5 s: the host's whole state.
  Tested (2 PCs, 100 ms lag): the guest 0.1-0.2 s after the host in capturing, captured, destroyed, expired; the team
  points the same. Test pipe: `pointon <id>` (the host's point on); FGUYMOD_TEST_CONF can carry the players
  (`IN,FR,0,0,0,0,0|Peter:0:0:c;Peter:0:1:c`: teams).
- Capture the Flag (mode 5, `0x6655a0`): the player's update `0x69b5f0` looks for the flag near (`0x66ce50`: player,
  position) and with the interact action (`0x15`) picks it up (`0x690110`: player, flag - `[player+0xeb2c]` = it;
  the flag `0x68f550`: its carrier `+0x70`, event `0xb92a5c53` with the team, the sting `MPXX_STING_CTF_pickup_ref`).
  "FGUY: MP: Flag Was Consumed or Killed" `0xabafef45` (sent from `0x66ceff`), the stat "Captures".
- The Deaf Guy is a character (`zNPCGeneric`, kind `3d:18e`) - spawned by the host's game in the new netcode like the
  enemies. Down, his carryable (`zCarryableObject`, vtable `0xa228e4`, id `0x4e` on Frat Row on every PC; the list
  `[0xbad994]+0xad8`, nodes at +4) comes on where he lies: `+0x7d` 1, `+0x84` its physics shape (`0x644350`); carrier
  `+0x70`. Grabbable (`0x6293b0`): shape, `+0x7d`, `+0x7b`/`+0x7c`, not `+0x40`; near (`0x478490`): distance and a
  ray. The X button picks it up (action `0x15`). Dropped: `0x66f320` (fastcall: the player) - down, captured, respawn.
- The bases: `zCarryableDropZone` (vtable `0xa220c4`, type `0xe5`; Frat Row `0x413f` team 1 at (120.2 7.6 83.0),
  `0x413e` team 2 at (49.5 8.4 204.9); centre `+0x74`, config `[+0x40]`: team `+0x58`, scores `+0x54`/`+0x55`, points
  `+0x4c`). Their update `0x68f660`: a carried carryable whose carrier is of its team and in it (`0x62a320`, thiscall:
  base, position - its only caller) - the carrier's score `+0xe84` += points, captures `+0xe8c` + 1, the team's points
  (`0x673ba0`), the sting, dropped (`0x66f320`); then the Deaf Guy again.
- In the new netcode (0.8.18): `'f'` from the host - each carryable's carrier and a count of pickups, each base's
  captures; on the others their own pickups are dropped (the host's game picks up for that player, ~0.2 s after the
  press), a new pickup of the host's taken at once, a carrier apart for 0.7 s without one: the host's; a base answers
  "in" only for a capture the host's had (within 3 s) - its own game plays it out. Tested (2 PCs, 100 ms lag, teams):
  the guest's pickup and capture, the host's, the carrier down - the same on both, scores the same. Test pipe:
  `killnpcs` (the host) knocks the Deaf Guy down.
- Research: FGUYMOD_TEST_NPCLOG=1 also logs every game mode event (hash, sender, arg), who switches the points, and
  the timelines' events.

## Story co-op in the new netcode (2026-10-22, research)
- The engine is Heavy Iron Studios' (the one of SpongeBob: Battle for Bikini Bottom - its fan decompilation documents
  it): the level's logic is objects (`xBase` / `xEnt`; struct types `zScript` - the timelines, vtable `0xa22224`;
  `zTrigger` `0xa21cfc`; `zConditional`, `xCounter`, `xTimer`, `xGroup`, `zDispatcherData` `0xa227cc`, `zPlatform`
  `0xa19844`, `zEntSimpleObj` `0xa21ab4`, `zDestructible`) wired by events: every event goes through the dispatch
  `0x520c50` (cdecl: from, from-event, to, event, args (floats), only-if-on), which passes it on along the target's
  links (recursively). An object: vtable, `+0x10` its kind, `+0x18`/`+0x1c` its asset id (the same on every PC;
  `0x616c60(lo, hi)` finds the object), `+0x26` flags (bit 0 on), `+0x28` links, `+0x30` event handler.
  Classes: `zFamilyGuyPlayer` `0xa1af14` (kind 0x6f), `zNPCGeneric` `0xa1aafc`, `zCheckpoint`, `zCinematic`,
  `zObjective`, `zTriggerEntity`, `zInteraction`, `zPortal`, `zSpringboard`, `zHazard`, `zNPCSpawner`,
  `zFGUYOneLinersEntity`, `zSaveLoad`; remnants of the original game's own networking: `zNPCNetworking`,
  `xNetProxy`, `zNetPlayer`, `FGUYPlayerLocalAuthorityUpdateMessage`.
- Events started fresh by the game's code (dispatch depth 0; FGUYMOD_TEST_EVLOG=1 logs them): every frame
  `0xafc4a7fd` to every script; a player entering / leaving / in a trigger (from the player, to the `zTrigger`:
  `0x22475de5/6`, `0x8a812dd8`, `0x8a812cc5`, `0x395f3819`, `0xe215652f`, `0x8a84d842`, `0xdd3d4b1f/20`,
  `0xd1d0ea3a/b`, `0xd1d12d43/44`, `0x36599286`, `0x365d3cf0`, `0xbbd19233` - sent from `0x4b1e7a`-`0x4b27ff`,
  `0x4cb26a`); scripts' timed events (`0x635f90`); the NPCs' own. Greek, the host walking: both PCs had the same
  trigger events at the same moments. Dialogue (one-liners) is picked at random on each PC: different lines.
- Design: a player's own events (depth 0, from that player) are its own PC's - sent, and the others drop theirs and
  play the owner's (so triggers, interactions, pickups go by where the player really is: respawn jumps, skips); the
  level's own logic (scripts, timers, what events set off) runs on every PC as vanilla; enemies and random picks the
  host's.
- The game's mode `[0xaee080]` (the one before it at `+4`; switched by `0x998f40`): 7 playing, 2 its pause menu and
  also a level's results. The pause menu opening plays `SNDS_PlaySoundFX(EnterPause)` (the results don't): the mod's
  sign that the game paused itself (it does sometimes right after a cutscene skip - Chicken Fight's between rounds).
- Cutscenes: the game's own "running" test (its tick `0x69f400`, before START may skip): the current one `[0xbad99c]`,
  its player's flag `[+0x7c] +0x38`; skippable: also seen (`+0x88`, or `[0xbad69f]`) and its own `+0x89`. The state
  `+0xac` is 3 for most but 2 for Chicken Fight's. START skips them through `0x76` (the pad's action) per active player.
- Levels the game loads by itself (no menu command): the results' CONTINUE (`GAME_Unpause`, `MENU_Stop`, then the
  next level) and Chicken Fight's "If you join, this level will restart" (the one-player `GCXX` -> two-player `GCMP`).
  A restart (`SCEN_Reset`) doesn't open the level's package again; a load opens it ~8 times, the game ticking between.
- A one-view level after a two-player one keeps the second player's object with its old viewport number (1) while the
  game has one view: not in yet. Player health: `[player+0x105c] +0x50` everywhere; `[player+0x5c] +0x140` is the
  same object except Peter's in Chicken Fight.
- Level 10 (`BPXX`) opens with the T-rex (`3025`, 5600 health, bullets on its body do nothing - "Destroy the T-Rex's
  Lasers", then Bertram's helmet, a 10-health character riding it). Not played through yet.

## Cosmetics: skins and parts (2026-10-22)
- Characters live in `FGUY/Global_Scenes/Players/PLYS.ho` (every character and outfit; PLYW / PLYN / PLYX subsets).
  `tools/re/ho.py` reads .ho packages, `tools/re/ho2glb.py` turns a `<Character><Outfit>_Rig_Geo` Model into a .glb.
- A Model: its SkeletonBlob and SkinGeometry pieces. A piece's record: +0x54 vertex count, ids of its Material and
  three RawBlobs (vertices - the biggest; texture coordinates, half floats, 4 bytes a vertex; triangles, u16). Vertices:
  position, normal, [tangent + sign], then for more than one bone 4 weight bytes; one table in the record of vertex
  counts by bone count (dwords k << 16 | count, counts with none left out), one-bone vertices first.
- Textures: a Texture asset (16 bytes) names its DDS blob. In the game: the asset's scene tree node -> a
  World::TextureResourceEntity (vtable 0x9fb2b8), [+0x28] +0x14 the IDirect3DTexture9 (managed pool).
- A character's color texture is bound on stages 1-2; at its DrawIndexedPrimitive the vertex shader's c4..c6 .w are its
  world position (which player it is). A player's model: [[player +0x38] +0x1d4] = its World::ModelPrototypeEntity
  (vtable 0x9fad48). The main menu's couch family aren't players; the lobby's 3D character is the menu's player 2.
- The outfits of one character have the same skeleton (1-4 header dwords differ), so pieces fit across outfits. A
  piece's runtime World::SkinGeometryEntity (vtable 0x9fb30c) has Direct3D objects at +0xac and [+0xc8] - present for
  every outfit in the package even when nobody wears it (the main menu): a piece of another outfit can be drawn.

## Rigs and motion (2026-10-22)
- SkeletonBlob: +4 u16 bone count, +0x14 offset of the parent table (a byte a bone, 0xff root), +0x18 the mirror table,
  +0x20 the matrices: a 3x4 row-major inverse bind matrix a bone. A vertex moved by one bone: its last dword a u16, that
  bone's matrix offset (bone * 48); by more: its bytes the bones, the dword before them the weights (`tools/re/rig.py`).
- Characters are skinned on the CPU, not in the vertex shader (which gets c0..c10 only): every frame, 0x4b02d0
  (__thiscall piece, instance) skins one piece - a per-frame Graphics::SkinGeometry render instance (vtable 0x9fcb78;
  +0xb0 bones, +0xb4 the bone palette) - into a staging buffer that 0x44da30 copies into one big dynamic vertex
  buffer (positions, 12 bytes a vertex). Instances and palettes are frame-allocated: no lasting pointer to them.
- The pose: the animation blend tree (0x522fb0) samples and blends into a local pose - per bone a rotation quaternion
  (x y z w), translation and scale in the parent's space, as glTF animation channels - and calls
  0x4fac80 (cdecl entity, rotations, translations, scales, bones). That builds the model-space bone matrices (down the
  parent table of [[[entity +0x1d4] +0x80] +0x283c]) into the palette; 0x42c330 / 0x4af270 multiply in the inverse bind
  matrices. The entity is the player's [player +0x38]. The game's pose has one bone more than the SkeletonBlob: its
  bone 0 is the root (the entity's origin), its bone n + 1 the skeleton's bone n.
- `src/loader/motion.cpp` hooks 0x4fac80: test pipe "poserec <slot> <frames>" saves a player's pose each frame
  (FGUYMod\motion_<slot>.bin), "poseplay <slot> <file>" plays one on that player instead of the game's own (looped,
  interpolated), "posefreeze", "posestop". Tested: Stewie standing, alive, drawn in a recorded knocked-down pose - his
  gun follows his hand. `tools/re/motion.py` reads a recording; `tools/re/motion2glb.py` adds it to an ho2glb .glb as a
  glTF animation (three.js plays it: the same pose as in the game).
- ComboAnimBlob (the animations themselves, e.g. STEWIE_Walk): a compressed format, not decoded; a watchpoint on one in
  the loaded package saw no reads while walking - sampled from a copy, or the data unpacked elsewhere at load.

## New characters: drawing our own model (2026-10-23)
- A character's draw (DrawIndexedPrimitive): stream 0 its skinned positions (float3, the per-frame dynamic buffer),
  stream 1 its skinned normals (float3, as TEXCOORD1, the same buffer), stream 2 its uvs (half2, TEXCOORD0, static),
  16-bit indices; its color texture on stages 1 and 2 (the material names it twice); stage 0 something else (our colors
  there turn it black). Each piece is drawn twice at the player's position (c4..c6 .w) and once in a pass whose
  constants hold no position (c4..c6 .w 0, 0.5, 0).
- A player's pieces: their model entity ([[player +0x38] +0x1d4], World::ModelPrototypeEntity) -> [+0xa4..] +0x12c (and
  [+0xa4] +0x3c) -> World::SkinGeometryEntity (vtable 0x9fb30c): +0xac its index buffer, +0xb8 and +0xbc its
  triangles, +0xc0 its index count (measured: Peter's body 5170 / 5170 / 15510). A draw of a worn model's player's piece (its index buffer one of theirs) is ours instead.
- The sketch (ink) lines aren't drawn from the pieces' draws: 0x4c6ed0 (cdecl, 9 args; its 2nd a scene node: +4 flags,
  +8 a Graphics::Renderable whose [+0xcc] is its world matrix, 3x4) makes something's lines when its flag 0x4000 is on
  (0x4a4a10: the silhouette test - faces toward / away from the camera). Off for a renderable at a worn model's player's
  position: no lines of theirs.
- src/loader/models.cpp: a .fgmodel (tools/re/glb2fgmodel.py, the Skin Studio's Models tab) skinned on the CPU from the
  player's pose (motion.cpp) and drawn with the game's shader. Tested: Stewie's own tux model tinted, a box robot from
  a .glb - stands, walks, emotes, no trace of the original.
- Skin packs of format 2 (skinbuild.cpp): the recolors and an overlay of the maker's own pixels; the texture made on
  the player's PC from the GPU's copy of the game's (DXT1 / DXT5 read back, the Studio's recolor, re-encoded).
- Worn over the character (model.json "over": true): ours is drawn with their biggest piece, then the game's own draw
  goes ahead; their other pieces and their ink lines stay. A skin pack can carry a model pack (model.fgmodel, stored):
  put on and taken off with the skin - online it travels in the same pack. Our pieces get no ink lines.
- Hiding the body under an outfit: model.json "hide" (the Studio: triangles of the character with two corners inside a
  solid part of the model - thick 0.12+ every way, 0.006+ from its surface; shells like a vest hide nothing), by
  piece (index and triangle counts) and number. The game reads the piece's own index buffer back (D3DLOCK_READONLY,
  INDEX16), collapses those triangles in a copy, and puts the copy in for that piece's draws (the skin's texture swap
  still applies: cosmetics.cpp draws, then puts theirs back).
- Copying a character's weights onto clothes: the nearest vertex is wrong for a bulging garment (a boot's front took
  the shoe's foot bone); the vertex beneath it along the garment's normal is right. Pieces on pieces (laces on a
  boot) take their host's weights, or they sink into it as it bends. The Studio's recolour must shift saturation, not
  scale it (a near-white pick made tinted pixels fully saturated).
- Stage 0 under a character's draw: one 128x128 A8R8G8B8 texture (8 levels) for all their pieces - a shading lookup,
  not an atlas; the colour is stages 1 and 2.
- The Studio's palette must be sRGB: three.js (GLTFLoader) keeps material and vertex colours linear, and written into
  the palette PNG as they were, every colour came out about squared (a vest of 98, 63, 59 drawn 26, 11, 8).
- Ink outlines: the game makes its sketch lines on the CPU from its own pieces (0x4c6ed0), so a model's get none.
  models.cpp draws each model again pushed out 0.009 along its normals, the other faces culled, a black texture on
  stages 1 / 2, the game's shaders (so depth / shadow passes just see it slightly bigger); parts under 0.07 across
  (laces, eyelets) left out - all outline, they were black blots.
- Our own pictures in the game's Scaleform menus (icons.cpp): a movie script's MovieClip.loadMovie(url) goes through
  the FileOpener; a url no package has (ours: "fguyicon_<key>.gfx") is served by opening one of the game's small
  movies again (noSpaceError.gfx, 496 bytes) and pointing its memory file at our bytes. This Scaleform only takes
  movies with GFX_ExporterInfo (tag 1000; the game's: version 0x36f, flags 0, bitmap format 13) - without it, "FWS"
  or "GFX", nothing loads. It draws no bitmaps of a movie's own (DefineBitsLossless2 shows nothing: the game's
  pictures are DefineExternalImage2 names its texture system resolves, never files), so a picture is one
  DefineShape2: flat colours, each border between two colours one edge carrying both fills (no seams).
- The model's outline is drawn with a depth bias away from the camera (0.0006, slope 1.5): without it the inside-out copy
  showed through its own surface in creases and round small parts (black dashes on the boots, smudged laces).
- Our HUD layers each need their own depth on _root (createEmptyMovieClip at a depth replaces what's there): the
  online ping display at 29990 wiped the emote wheel drawn at the same depth. And online the HUD's root is zoomed to
  one viewport: a layer of ours in screen units undoes it (FGUYMouseUnzoom).
- Online, the test pipe's pad (hold / press) is added after the emote wheel has seen the real controller
  (netplay.cpp SampleLocal): with no controller there, it now goes through the wheel too.
- The lobby server passes a player's custom skin (SKIN <its head picture's id>, a member's 10th field, only to a server
  whose WELCOME says "skins"); a PC that has that skin shows its head in the player's round portrait.
- The HUD's head: Icon_Lifebar<GetObjectPath(viewport, player)>.PlayerHead, a frame per character
  (SetHeadIcon(playerIndex, character)). Our head is a clip laid over it, PlayerHead hidden.
- The lobby's character screen (lobby.as2js): a skin's tile follows its outfit's; its picture loaded the same way.
- The Studio's rest pose has the characters' eyelids shut (the game animates them open): the triangles the "Face"
  bones move are left out of its pictures.
- First outfit made with img2threejs: content/peter-adventurer (its README) - fitted on Peter's measured bind mesh,
  16,720 triangles (Peter: 8,680). Tested in solo Multiverse Madness at 1920x1080 (tools/test_solo_mp.ps1 -God).

## Graphics: the blur, filtering, post passes (2026-10-23)
- The PC game has **no anti-aliasing**: every render target, depth buffer and the back buffer are made without
  multisampling, whatever the embedded ini's `MSAAType` (4) says - shots with 4 and 0 are the same.
- Its render targets at 1920x1080 (FGUYMOD_TEST_RTLOG): screen-sized A8R8G8B8 (several) and R32F (depth for its
  effects), 960x540 / 482x272 / 240x135 (glow and blur chains), 512x512 and 256x256 (shadows), and a 1920x1080
  StretchRect copy (no filter) every frame.
- **The blur**: the port never made up for Direct3D 9's half-pixel offset, so every full-screen pass reads its
  screen-sized targets half a texel off and bilinear filtering smears the picture, pass after pass. Point filtering on
  those targets (the first try - and what the community's dgVoodoo2 "point sampling" fix does) sharpens it but breaks
  the passes that blend on purpose (ambient occlusion, blur, glow: the characters' shading and colours changed,
  measured in the main menu at 4K). The fix that keeps them: every vertex shader (all 218 are vs_3_0) patched at
  CreateVertexShader to end with `oPos.xy += c255.xy * oPos.w`, c255 = (-1/viewport width, 1/viewport height) - set at
  SetViewport (aras-p.info, "Solving DX9 Half-Pixel Offset"). The game never uses c255. c255 = 0 is the game as it was
  (identical colours, measured), so Sharp picture switches live. At 4K: edge strength 3.9 -> 5.3.
- Anisotropic filtering and FXAA change little at 4K (edge strength 2.52 -> 2.57 on the ground; FXAA's steps are a
  4K pixel wide).
- `G.CameraFOV` in the ini changes nothing on PC (80 and 105 look the same): the levels' cameras set their own.

## The Xbox 360 build next to the PC's (2026-10-29)
- Its DEFAULT.XEX: retail encryption + basic compression -> memory image at 0x82000000 (tools/re/xex.ps1); in Ghidra
  as PowerPC:BE:64:A2ALT-32addr with its .pdata's functions (tools/re/ghidra/import.ps1 x360). Its code saves registers
  through __savegprlr helpers the analysis takes for non-returning: Decompile.java clears that and uses .pdata bodies.
  Xbox code builds addresses with lis + addi: tools/re/ppcxref.py finds them.
- Its built-in ini = the PC's but for `MSAAType = 2` (PC: 4). The PC code never reads MSAAType (no reference at all).
  The Xbox reads it (0x825f33b0: 2 -> 1, 4 -> 2, else 0 = D3DMULTISAMPLE 2 / 4 samples / none into [0x82ac3910]),
  but its video setup (0x821be338, from the start-up list) overwrites that before it makes its targets: an HD TV gets
  1280x720 with no MSAA, an SD TV 640x480 with 4x (0x821bddd0 makes the multisampled scene colour + D24S8 depth). So
  the console on an HD TV has no anti-aliasing: a 720p picture, scaled to the TV by the console.
- The same effects on both (constant names of the shaders in BULK.ho / MNUS.ho): bloom, depth of field, SSAO, the
  ink outlines (jump flooding, tJFA), an edge filter (e_kernel); the Xbox adds Kinect ones and VSM shadow blurs.
- The main menu draws (FGUYMOD_TEST_PSTRACE, graphics.cpp): a 512x512 shadow map and its blurs, the characters
  (opaque, stage 0 the 128x128 shading lookup - 8 mip levels), the outline shells (inside-out, no depth bias), projected
  shadows, bloom at 960x540 / 482x272 / 240x135, its composite, then Scaleform. No SSAO there.
- **The grainy grey round the characters' eyes, lenses and creases** (the "dark thing"; not on the Xbox in Xenia): the
  characters' ink-line / soft-shading pass (pixel shader `FogColor,diffuseMap,lightMap`, blended, stages 0 and 1 a
  16x16 L8 ramp the game makes in code: 255 - 17 * max(x, y)) reads that ramp's small mip levels (down to a 1x1 of
  grey 83) wherever its coordinates change fast. The Xbox makes the ramp with one level (0x821b5830:
  CreateTexture(16, 16, 1, 0, L8 0x28000102, ...)); the PC port with five. Fixed with Sharp picture: the ramp's mip
  filter none (graphics.cpp, the ramp fix) - the PC then matches the Xbox shot. Ruled out on the way: our texture
  filtering, projected shadows, outline shells, the 128x128 shading lookup (its mips, its shadow tone), the palette
  strips' mips, the resolution. The shipped game has it too, under its blur.
- **The characters' outlines thin out at high resolutions** (the Xbox's look thick: 720p, scaled up). They're the
  ink strokes (the same pass: FGUYMOD_TEST_INKRED=1 draws them red) - only the outlines (each character's edge, the
  eyes, glasses, Stewie's head); the lines inside (smile, chin, creases) are in the models and scale by themselves.
  The game makes the strokes each frame as quads in view space (32-byte vertices: position, D3DCOLOR, uv0 along,
  uv1 across; the pixel shader draws ramp(uv0) * ramp(uv1) * alpha, each uv pair adding up to the same number at a
  quad's corners: a solid band with a one-unit soft edge). They're a few pixels across at any resolution, and fainter
  the taller the picture: colour ff000000 at 1080p, 4c000000 (30%: about 720 / 2400) at 2400 lines, uv1 +-5 then
  +-2.4. Line thickness (graphics.cpp): the strokes' vertex shader makes their alpha solid again (x height / 720) and
  the pixel shader widens the band (uv1 + c) - auto 1.5x at 2400 lines matched the Xbox. Growing the quads on the CPU
  (to fit wider lines) needed their vertex buffer read back each draw: 35 fps -> 7, so not done; a quad holds about 3x.
  The outline shells (FogColor,outlineColor) push out by outlineParams.x * the vertex's distance (capped by .y): a
  fraction of the screen, so they scale already.
- Research switches (graphics.cpp, environment only): FGUYMOD_TEST_PSTRACE=1 (every 1000th frame: each pixel shader by
  its constants' names, its render target, blend / depth states, textures and their filters, the game's return
  addresses from the stack), FGUYMOD_TEST_PSSKIP=<constant name> or =<whole name list> (draws with that shader left
  out), FGUYMOD_TEST_PSCONST=name[=v0,v1,v2,v3];... (a constant logged / held). The menu's light changes by itself
  (the TV): compare colours only within one run, switching live (pipe `gfx`).
- Our own passes at Present (FXAA, sharpening, brightness) compile with d3dcompiler_47.dll's D3DCompile (ps_3_0) and
  run on DXVK and plain Direct3D 9 alike; a full-screen quad with XYZRHW at -0.5 lines up texel for pixel (checked).
