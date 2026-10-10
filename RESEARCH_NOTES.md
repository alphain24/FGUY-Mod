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
