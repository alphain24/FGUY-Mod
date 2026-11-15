# Family Guy: Back to the Multiverse (PC) Ã¢â‚¬â€ Research Notes

Survey date: 2026-10-08. All findings from read-only inspection of the installed game.

## Executable (`FGUY.exe`)
- 32-bit x86, built 2012-11-12 with Visual C++ 2010 (`MSVCR100.dll`).
- SHA-256: `E4BCD19E208E2B17FF18E5B3BC37D6059FE8B4A6FC03CDDC0E86CE930F74E42B`
- Not packed or encrypted (normal section entropy, no SteamStub `.bind` section).
  Imports `steam_api.dll` normally: the Steam build of the game.
- Contains C++ RTTI class names (e.g. `.?AVNetworkModule@IO@@`) and source paths
  (`C:\branches\FGUY\main\NG\Source\Engine\...`) Ã¢â‚¬â€ very helpful for reverse engineering in Ghidra.
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
- `BOOT = /FGUY/Levels/Level_FRXX/FRXX` Ã¢â‚¬â€ level to boot into (useful for testing custom levels)
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
  parser is the function around `0x99d3a0`Ã¢â‚¬â€œ`0x99db3c`.
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
  `EnableNetworkPlay = 0` Ã¢â‚¬â€ the first runs just didn't log sockets yet. So `EnableNetworkPlay` controls
  something else (still unknown).
- The game opens **TCP listen on 0.0.0.0:7534** and sends a **25-byte UDP broadcast
  to 255.255.255.255:7533 once per second** (LAN "I'm here" beacon). Game keeps running normally.
- Beacon bytes: `19 00 00 00 | 0a 0b 00 00 | 00 | 6e 1d (=7534) | 00 ... 01 ...` Ã¢â‚¬â€ length, message id,
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
  (offset `0x3da72`). Skipping the attract-mode trailer was fine. To investigate (Bink Ã¢â€ â€™ XAudio2 teardown).

## Menu flow (controller mode, 2026-10-09)
Intro movies Ã¢â€ â€™ attract trailer (if idle) Ã¢â€ â€™ title "press start" Ã¢â€ â€™ **Select Save** (3 slots; X = delete!,
B = "quit to title?") Ã¢â€ â€™ **Main Menu**: Story Mode, Multiplayer, Challenge Mode, Options, Store,
Bonus Materials, Exit Game.
- **Multiplayer**: Deathmatch, Multiverse Madness, Capture the Greased-Up Deaf Guy, Infiltration!
  (level suffixes DM / HD / CF / IN). No network/online entry in the vanilla menu.
- **Match setup** (Deathmatch): Map, Time Limit, Winning Score, Teams/FFA, Friendly Fire Ã¢â€ â€™ Next.
- **Join screen**: 4 panels, "Press A to join" per controller, then Select Team (Good/Evil).
- The game autosaves the selected slot on menu transitions ("Saving..." icon).

## Menus: Scaleform GFx 3.x (2026-10-09)
- Menus are Scaleform GFX movies (Flash 10 / **ActionScript 2**), uncompressed inside the `.ho` packages.
  `main_menu.gfx` (290,635 bytes + 1 pad byte) is in `Global_Scenes/MNUS.ho`. Other movies seen:
  pause_menu, stats_screen, objective_screen, Dialog, trcMessageBox, start_screen, credits, icons, cursorsÃ¢â‚¬Â¦
- `Scaleform::FileOpener::OpenFile(url, flags, mode)` = vtable `0x9fe670` slot 1 (`0x4421d0`); returns a
  `Scaleform::GSFMemoryFile` (vtable `0x9fe61c`; +0x08 path, +0x0C data, +0x10 pos, +0x14 length).
- Menu Ã¢â€ â€™ C++ calls: `ExternalInterface.call("CATEGORY_Name", ...)` Ã¢â€ â€™
  `Scaleform::ExternalInterfaceHandler::Callback(movie, name, args, argc)` = vtable `0x9fe56c` slot 1
  (`0x454190`). Return values go through the movie's vtable slot 50 (`SetExternalInterfaceRetVal`).
  `GFxValue` = { objectInterface*, type (2 bool, 3 number, 4 string; 0x40 managed), 8-byte value }.
- Categories seen: GRFX (graphics), OPTN, MENU, SCEN, SAVE, CTRL, PROF, SYST, LOKR, STOR, MULT, PLYR,
  FGUY, and **NETW** (`NETW_IsServer`, `NETW_SetActiveMenu`, `NETW_OnScreenEntered` are called by the
  main menu Ã¢â‚¬â€ the network layer is wired into the menus).
- Tools: `tools/re/swf.py` (tags), `tools/re/as2.py` (AS2 disassembler), `tools/ui/gfxpatch.py`
  (assembler + patch writer), `tools/ui/make_menu_patch.py` (the Borderless menu patch).
- Options > Screen script: `OnOptionsScreenEnter`, `GetDisplayModeValue`, `OnDisplayModeValueChanged`,
  `OnOptionsScreenNextButtonPressed` Ã¢â€ â€™ `GRFX_ChangeResolution(width, height, windowed)`.

## Window procedure and screen size
- Game window proc `0x483920` handles only WM_DESTROY, WM_ACTIVATE, WM_CLOSE, **WM_WINDOWPOSCHANGED**.
  On WINDOWPOSCHANGED in windowed mode it sets the screen size globals (`0xb06360` width,
  `0xb0635c` height, `0xb06354` windowed flag) from the window's client size Ã¢â‚¬â€ even for SWP_NOSIZE.
- `0x419680(windowed, x, y, w, h)` applies a resolution (called from `0x4cac63`); the Options page has a
  "keep this resolution?" timer that reverts after ~16 s.
- DirectInput keyboard cooperative level is `0x16` (includes DISCL_NOWINKEY Ã¢â€ â€™ Windows key blocked).
- Intermittent startup crash at `+0x876e9` (NULL D3D device). Loader now retries `CreateDevice`.

## Leftover netcode verdict (2026-10-09)
Not a usable online mode. `IO::NetworkModule`'s vtable is mostly `ret 4` stubs (`0x7af040`); the only
gameplay message (`FGUYPlayerLocalAuthorityUpdateMessage`, built in dead function `0x647d50`) is never
sent. The TCP 7534 / UDP 7533 beacon is most likely the developers' editor link (GoodEditor,
`EnableRealTimeUpdate`). Network ini keys sit with other dev-only options (watermark, soak tests).

## Players
- Class `zFamilyGuyPlayer`, vtable `0xa1af14` (167 slots); base/sibling `zNetCommonPlayer` (`0xa8e49c`).
- `+0xdf8` player index, `+0x38` Ã¢â€ â€™ transform, position at transform `+0x30` (x, y, z floats).
- Story level FRXX has 3 player objects (index 0 Stewie, 1 Brian AI partner, 2 unknown/inactive).

## Determinism / lockstep findings
- Random: `srand` seeded from QPC at startup (`0x4ca150`); more RNGs: Mersenne Twister (`0x4f6790`,
  `0x49ed60`), LCGs (`0x41f260`, `0x4f67e0`, `0x6fab40`, `0x8601b0`, `0x8c3050`).
- Time: QPC used 108Ãƒâ€”, GetTickCount 12Ãƒâ€”, timeGetTime 4Ãƒâ€”; `LockFramerate = 0` (variable dt).
- Threads (2026-10-10): the MAIN thread (the one that loads dinput8) runs game logic. Each tick:
  wait for the render thread's go-ahead, poll XInput pads 0..18 itself (`0x4cc4a0` Ã¢â€ â€™ `0x4b5670`;
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

Changing the size (2026-10-30): `GRFX_ChangeResolution` queues `0x44f920` Ã¢â€ â€™ render thread `0x4cac40(w, h, windowed)` Ã¢â€ â€™
`0x447950` (same windowed state) / `0x419680` (windowed toggle): they fill the present parameters (`0xaf3418`), set
"reset asked" `0xb06c12` and call the device reset `0x420880` (resources let go, TestCooperativeLevel, Reset through
`0x48b760`, made again), keeping its answer in `0xaf35a1` (the frame `0x44da30` retries while it's 1). Scaleform's
renderer (`[0xb06c4c] + 0x3e0`, release `0x7125d0`) only lets go of its DEFAULT-pool buffers / glyph / gradient
textures when TestCooperativeLevel isn't D3D_OK, so a reset asked from the menus was refused (INVALIDCALL - 33 of
its resources alive, `FGUYMOD_TEST_POOLTRACE`) and, its answer 0, never tried again: the picture froze for good. The
mod answers DEVICENOTRESET while `0xb06c12` is set. The game's Present is `0x4a39c0` (EndScene, StretchRect, Present).

## File loading
All game data is opened via `CreateFileW` with relative paths like `./FGUY/Levels/AMDM.ho`, so the
loader can redirect individual files to modded copies later. The game also probes `./boot.ini` (not present).

## Data files (`FGUY\`)
- `.ho` Ã¢â‚¬â€ main asset/level packages (69 files, ~2.5 GB). Magic `HEL\x1a`, version 1, build timestamp
  at 0x10, data begins ~0x400. Contents are **uncompressed** (readable strings, Havok `hk_2010.1.0-r1`
  physics blobs, AI behavior-tree names, tool tag `GoodEditor:PackView.GEPlugin`).
- `.fsb` / `.fev` Ã¢â‚¬â€ FMOD sound banks / event files (standard, existing tools can open them).
- `FMV\*.BIK` Ã¢â‚¬â€ Bink videos.

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
- **A model's black eyes** (a skin's model from a character's own export, 2026-11-01): a character's eyes are two
  pieces each - the lids (<Char>_Lids_CO, 1024x128, an atlas of 16 lids: an open one see-through, alpha 0 and black)
  over the eyeballs (<Char>_Pupil_CO, white with a pupil, opaque), the eyeballs about 0.005 behind on the same
  shape. Our draw uses the body's shader, whose output alpha isn't the texture's: an alpha test or culling changed
  nothing, the open lids drew black. models.cpp MergeLayers: at load, a piece under a see-through one (its vertices
  each within 1/20 of its size) is painted into its see-through texels and not drawn. The Studio draws them with an
  alpha test, so it showed them right. Research knob FGUYMOD_TEST_MODELSKIP=<bits>: those parts not drawn.
- The skin pickers' pictures are Costume_<Char><Outfit>.tif (MNUS.ho; Adam West "West"), with some outfit names not
  the models': cosmetics.cpp PictureOutfit / model.js PICTURE_NAMES. A skin made on a model name the picture doesn't
  use (Stewie "SexyPartySuit" - picture "Sailor") had no row in the Store.
- The Store sells an outfit only once its challenge is done (IsCostumeLocked -> STOR_IsCostumeUnlocked, the game's)
  and it isn't bought; the research save has bought every outfit it may. Research knob FGUYMOD_TEST_UNLOCKED=1
  (ui.cpp): every outfit unlocked, so buying can be tried. Tried 2026-11-01: a custom skin's locked row (Chris
  Christobel, multiplayer costumes) charged 2000 once and both rows came out bought, the skin on the outfit.

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

## First person: the camera's height and distance, aim (2026-10-31)
- The player's camera orbits a point straight above the player (x, z the player's) at a height and a distance from a
  camera tuning block: `[player+0xfa8c]+0xc4` = **height** above the feet (Stewie 1.2), `+0xcc` = **distance** (6.0),
  `+0xc8` (0, between them) still to try. (The same block is reached from `[player+0xfa94]+0x8c` and `[player+0xff8c]+0x29c`.)
  Writing them moves the game's own camera at once (distance 0.05, height 1.0: Stewie's eye level) - aim and the
  view follow, since they come from that camera. Two players of one character share it (and the settings blob).
- Not these: `camera+0x248` (6.0, a target recomputed every frame) and the orbit block's own distance `+0x274`.
  The settings at `[camera+0x178]` (FOV 1.396 at `+0x88`, pitch limits -0.785 / 0.785 at `+0x94` / `+0x98`) don't hold them.
- Aim: `0x5ec750` (a player method) - when `+0x14d0 == 2`, `0x5d85f0` (the reticle's point on screen) and `0x6568c0`
  (viewport, point, out): a direction only, from the viewport's projection (`viewport+0x164`) and inverse view matrix
  (`viewport+0x3a28`); otherwise the direction stored at `+0x14d4`. The camera stays level: the reticle moves instead.
- **Where the camera reads them: 0x647890** (this = the player, ret 4, arg != 0: mode 0) - the player's camera mode
  `+0xed08` (0..4) picks the character's offsets through 0x62b730 / 0x6316a0..e0 and hands back a pointer to them:
  side +0x0, height +0x4, ahead +0x8, distance +0xc (= tuning block +0xc0..). The getters 0x647a70 / 0x647ad0 /
  0x647b30 / 0x647930 blend them between modes (`+0xeda0` blending, `+0xeda8` how far, `+0xedac..` the old values).
  Its callers keep edx and the SSE registers across the call (the game's own never touches them) - a hook must too.
  Found with a read watchpoint on the game thread only (src/loader/firstperson.cpp "fpv watch"); the old watch (every
  thread) crashed the game. Repointing the player's +0xfa8c / +0xfa94 to a copy changed nothing: nothing else points
  at the block - it's reached by arithmetic from the character's data.
- The camera won't go below a height that's about the top of the character's collision (Peter 1.80: asking for less
  gives 1.80 and the distance 0.1); above it, anything.
- The right stick pitches the camera (limits from the settings, `+0x94` / `+0x98`); the stick click (R3, bound to
  actions 26 / 52 / 53, keyboard C) moves the camera too. Pad buttons in the game's bits at device+0x14 (a word; R3
  0x0800, L3 0x400, A 0x1000, Back 2); every pad bit is bound to some action; keyboard V is free.
- Ink lines are made from the camera's place (the game's camera, not the drawn one): a view drawn from elsewhere gets
  scribbles round the characters.
- Research scripts: the session's fpv_probe / fpv_find / fpv_ptrs / fpv_scan / fpv_copy / ptrscan (camera dump, value
  search, struct pointers, settings scan by poking, private copy test, every pointer into a range).

## First person: their own body in their view (2026-11-01)
- A character faces where they last went, not the camera: standing and turning the view in first person looked across
  their own shoulder and gun (Peter's drill gun is held up beside his head). Their heading: `[player+0x5c]+0x58` and
  `+0x78` (radians, 0..2 pi; +0x78 the one it turns toward) - the game rebuilds the transforms ([+0x38], [+0x5c], rows
  right / up / forward / place) from it every frame, so turning only the matrices did nothing. Writing both: the body
  follows the view exactly. The forward vector also sits at player +0x10a8 / +0x10b0 (x, z).
- The camera trails the player as they set off: the orbit block's point (`player+0xf3a8`, eye at +0x18) is written from
  a smoothed copy of its own each update (0.6 behind at a run) - writing the point before the update changes nothing;
  moving it (and the eye) after the update does, and the view follows.
- A character's draws carry world positions: they're skinned on the CPU straight into the world, c0..c3 is the view x
  projection (the eye solved from c0, c1, c3 came out as the camera's world place); c4..c6 is near their place but
  doesn't move the points.
- The skeleton's parent table at runtime: `[[[[entity +0x1d4] +0x80] +0x283c]]` (a byte a bone, 0xff the root; Peter 58
  bones: 0-6 the spine to the head, 6 with 11 face bones on it). The head bone (the one with the most bones on it) sits
  1.42 above Peter's feet, at the neck; the camera's eye is at 1.80. Setting off at a run, the head lunges ahead and
  down past the eye for about a third of a second.
- A skin's model drawn with a clip plane through it shows its outline (the inside-out black copy) through the cut: a
  black blob (the Adventurer's hat brim seen from below). Their own body's outline isn't drawn in first person.
- Offline Deathmatch with two pads (pad 1 joins) has no enemies - a calm place to test views. The test pipe's `heal
  <player> <health>` keeps a player alive offline (god works only free-running).
- The ink lines of a player's face aren't clipped with their body (drawn apart, from the queue of 0x4c6ed0): setting
  off, they hung in front of the eyes. Their own lines are left out while their head is half way to the eye or past it
  (with one view on the screen, always in first person - split screen, the partner keeps seeing them otherwise). The
  neck cut sits 0.15 under the head bone: the chin and an open mouth are below it.
- The game's own outline shells (pixel shaders with outlineColor) showed black through the neck cut too, looking down into the opened shoulders: on a player's own clipped body they aren't drawn. Peter's drill gun is held up at his shoulder, beside his head, even firing - from his eyes it's out of the view; a gun held at the chest sits under the view's bottom edge (77 degrees across at 16:9 is 48 up and down).

## First person: shooting, the gun's stances, zoom (2026-11-02)
- **The clip**: player `+0x4ffc` (int; the weapon objects at `[player+0x2240]` = `[player+0xf0]` keep the rounds used at
  `+0x164`). Shots are taken in `0x675740` (callers `0x67f230`, `0x6835f0`; `0x6755a0` / `0x675850` the clip's sums).
  Peter's drill: one round a pull. On the virtual pad every pull fired, first person or third, zoomed or not, in the empty
  arena; with enemies about, 94% (the misses: hit stuns) - no first-person cause found. What looked like "not shooting":
  the gun's kick swung the game's hand, our arm let go and the gun (and its flash) left the view.
- **The upper body's blending, `[player+0x13cc]`**: `+0x78` an action played over it (-1 none, 4 the aiming stance -
  firing, zooming, reloading, RB), `+0x50` its weight (eased in ~0.1 s, out ~0.15 s, ~1.3 s after the last shot);
  `+0x104` the relaxed stance's weight (it gives way to the aiming stance and to a melee swing, a jump, a power-up's
  throw). Player `+0xfa24` / `+0xfa28` follow `+0x50`. Between the two stances the hand swings 45-60 degrees against the
  upper arm - as much as a reload or a power-up's gesture.
- The hand against the upper arm, measured (max): standing 9.5 degrees; a shot 58 / 0.23 (for an eye 1.8 up); reload 48
  / 0.18; zooming in 60 / 0.29; Peter's melee 13 / 0.02; a power-up 65 / 0.14.
- **Controls (pad)**: LT zooms (held; the view's projection narrows - Peter's drill to 0.74 of its width - the camera's
  field of view, settings `+0x88`, doesn't change), RT fires, X reloads, B melee, LB a power-up, D-pad left / right
  pick the power-up (Multiverse Madness: one gun each, no weapon switch).
- **Vaults**: sprinting into low cover (Santa's Workshop's crates) vaults over it - the other arm reaches across the
  view and the eye goes through the crate's top for a moment. Not handled yet.
- Test copies on this PC lose their Direct3D device (0x88760868) a minute after the last real input: the screen saver
  (`Mystify.scr`, 60 s; the virtual pad isn't input to Windows). `tools/keep_screen_on.ps1` keeps it off meanwhile.
- **The ten Multiverse Madness characters** (character select from the left; it wraps after ten): Peter 59 bones (pose
  count), Lois 50, Chris 47 (a bow, in his LEFT hand), Brian 71, Stewie 48, Meg 61, Carter 53 (two revolvers), Quagmire
  66 (a rifle), Death 61 (a staff in his left hand; casts from his hands), Cleveland 44 (two pistols). Skeletons differ a
  lot: the most-branched bone is Meg's hips, not her head; Stewie's head has only his two eyes on it (his chest has more);
  Chris's and Cleveland's arms have no collarbone; cartoon hands have four or five fingers. What holds for all: the hands
  are a pair of alike bones (as deep, as many bones on them) with 4+ short finger chains; the chest is where the arms
  meet; the head is the chest's other branch nearest the eye, down its neck.
- Aiming (`[+0x13cc]+0x50` at 1), the game points any character's gun at the crosshair: the hand's turn then, against the
  view, is the gun's in first person - no per-weapon table needed.
- **First person's arms, take two (2026-11-03): the game's own animation, moved as one piece.** Bending the arm to hold
  the gun (IK) looked jagged and lost the game's moves. Now both arms keep the game's exact pose, hung on a chest placed
  in front of the eye (the chest in the view as the game holds the aiming stance, measured standing still; the whole
  thing moved so the gun hand comes out low right). The relaxed carry (Peter's drill up beside his head, a pistol at the
  hip) is useless from the eye, so in it - and on the move, where the game plays its own arm swing over both stances -
  the arms hold the aiming pose taken then; firing / zooming / reloading (aiming stance), melee / jump / power-up (neither
  stance, standing) play live. The pose builds every frame (~120/s, as drawn). The view's basis must be left / up /
  ahead (right / up / ahead is a mirror: quaternions from it are nonsense). Per character: Death low and far (his robe's
  sleeves), Quagmire further right, Chris's bow in his left hand.
- **First person as a shooter's viewmodel (2026-11-03)**: the arms and gun drawn in front of everything - their
  draws' viewport depth squeezed to 0..0.02 (a wall never cuts into them; among themselves still in order) - and with
  a field of view of their own (clip space x, y - c0, c1 - scaled by the view's tan / 0.79: the same size at any FOV,
  zoomed too). The gun is a model of its own (static vertex buffer, c4..c6 its place, near the gun hand); the sky dome
  sits at the eye too, told apart by its size. The rest of the body is hidden by shrinking the skeleton's top bones to
  0.001 at the chest (the arms' roots grown back by the inverse): **the game's pose build (0x4fac80) leaves the pose's
  root entry 0 out** - scaling it did nothing; a bone's matrix is scale x rotation + translation, times its parent's
  (row vectors), so a child's translation is scaled by its parents' scale. Hidden, the player is still hit (enemies
  killed Peter in 31 s). Split screen keeps the body (one pose for every view). Each character's aiming hold saved in
  FGUYMod\fpv_holds.dat (key: bones x 256 + gun hand bone): arms there from spawn.
- **What made the gun jump while walking (2026-11-03)**: not the game's walk - aiming on the move its gun hand stays
  within ~1 degree / 0.01 of its standing hold. While walking the upper body's weights are aim 0, relaxed 0.05 (the
  rest: the move's own arm swing); the arms were held on the aiming pose by `still ? relaxed : 1 - aim` - so at every
  stop (relaxed comes back over ~0.2 s) and whenever "still" flickered on mid-walk, for a moment the game's walking
  carry showed: Peter's drill swung up 60 degrees. Now the move's swing is held out until they've stood still 0.35 s,
  and while the game eases between aiming and relaxed (its arms between its two holds, the hand up to 25 degrees
  off) the gun hand is kept on its hold (both arms moved together - the off hand keeps its own moves).
- **The gun's draw (Peter's drill)**: a model of its own on the hand bone (unskinned; it and its outline shell drawn
  with the same c4..c6), but its own place is well off the hand - 1.3 at full size - so "near the gun hand (0.8)"
  never found it: it was drawn as part of the world, at the world's depth and field of view. Stood against a door the
  drill (an arm's length ahead of the eye) was behind it - hidden, the hand left alone; zoomed in or running (the
  view's field of view changed, the arms' not) its outline shell came out bigger than the drill - a black blob. Now the
  arms are shrunk toward the eye (0.35: the same on the screen, never through a wall) and the gun is found by that:
  an unskinned draw of the player's, shrunk by the same (its rows' length), within 2 x 0.35 of the hand. The sky dome
  (unskinned, at the eye, full size) is told apart by its size - taken for a gun, the sky went black. For the arms'
  draws the near plane is 10x nearer (c2.w: z = A w + B -> A w + 0.1 B): a reload swings the drill past the eye. A
  player's skinned draws have c4..c6 = identity at their feet.
- **A bone near the camera: the game doesn't draw the character at all** (none of their skinned draws). Stewie (eye
  0.96 up, 0.64 aiming): drawn with his nearest bone 0.17 from the eye, not at 0.10. So the arms are shrunk only as
  far as keeps every arm bone 0.22 from the eye (more or less makes no difference on the screen), and the hidden body
  stays at an arm's length (shrunk in at the eye too, Stewie wasn't drawn standing either).
- **Why the arms shook walking (2026-11-04)**: the pose is built 0.1-10 ms before its frame is drawn (traced: `fpv
  armstrace <frames>`), the eye on the move by then - 1-3 cm walking, at the hand's half a metre 6% of it, different
  every frame (the hand's frame-to-frame unevenness on the screen p95 0.027 of the half width, up to 0.087). Moving the
  arms' draws with the camera fixed it but left their outlines behind: a character's outline shells are drawn in a
  pass of their own, later, unskinned, at the world's origin - nothing says whose (Death's sleeves showed black). So
  instead the view is drawn from the very eye the arms were posed for (FirstPersonDrawnCamera): p95 0.00026; the world
  a few ms off is nothing at metres. The old bob (|cos| - a kink at each step's bottom, a fixed rate) is a stride's
  figure of eight by the distance actually gone, its size by the speed. The character's size (eye height / 1.8) is
  taken standing in first person, not aiming (the eye drops aiming - Stewie's by a third; going in it's higher -
  Quagmire's arms came out small).
- **The pops walking and standing (2026-11-04)**, measured with every frame of a burst (test pipe `shots <n> <folder>`,
  `tools/re/fpv_burst.py`, `fpv_drill.py`, `fpv_flow.py`):
  - *The idle fidget*: stood still ~6 s, the game plays a fidget over the relaxed stance for ~3 s - its weight
    (`[player+0x13cc]+0x104`) to 0.02 - and the arms let "neither stance" through as for a melee swing: the drill turned
    64 degrees and back. The fidget's own weight is **`+0x48` (float, eased 0..1; `+0x90` 2 while it plays, 3 otherwise)**
    - 0 firing, in a jump, a swing: held out by it.
  - *Threads*: the pose is built on the game's thread (~200 a second) and the frame drawn on the render thread at
    their own times; a frame shows the newest pose - one pose twice, or one skipped. Drawing the view from the pose's eye
    stood the view still on frames without a new pose (15 of 119 strafing); now the view is the game's (smooth) and the
    arms' draws are moved by camera-drawn x pose-camera^-1, each draw from its own c0..c3 (its eye solved, c3 its way
    ahead) - an outline shell is drawn in a pass of its own. The pose's numbers taken once a frame (FrameArms): a gun drawn
    from one pose and its outline from the next were a turn apart spinning (its outline's inside over the drill's face).
  - *Outline shells* (unskinned, at the world's origin, nothing says whose): split at 1 m from the eye - within, drawn as
    the arms (moved, in front); beyond, as the game. The arms' depth is their own (0.02..3 m into the front 2%): the
    game's near plane brought 10x nearer was too coarse there.
  - *The bob*: the old one was 0.4 degrees - the drill didn't move running (0.01 px p95). Now it's the game's: the head's
    bounce against where it stands, 0.25 of it (walking the head goes 8.5 cm up and down twice a stride, 5 cm side to
    side once; running 27-29 cm at 3.1 a second), held to 0.25 m/s and eased twice (the run's pose jumps up to 12 cm),
    kept with its time and drawn at the frame's (`MotionOffsetAt`, 12 ms behind): 0.8 deg walking, 1 deg running, its
    change frame to frame p95 30% of a step. Without a bob, running, the drill stays within 0.1 x 0.2 px.
- **The game's own arm moves on the move (2026-11-05)**, `[Camera] FirstPersonMoves` (test pipe `fpv moves <0|1|2>
  [<swing>] [<log>]`): 0 the aiming hold, steady; 2 the game's arms as it plays them - its walk and run are the relaxed
  carry's: the gun hand 69 degrees off the aiming hold on average, at a run Peter's drill on its side along the bottom of
  the view, flipping back at each stop (range 23 x 62 px of 320 x 180, a frame's worst jump 9.7); 1 the hold and the
  game's swing on it: the gun hand against its average over 0.35 s (the rhythm and size of the stride, not the carry it
  swings about - 6.7 degrees and 5 cm on average, up to 14), half of it, the average caught up first for 0.3 s after
  setting off, eased in and out (range 8 x 19, worst 3.3; walking as even as the hold).
- **Small arms (2026-11-05)**: a stance's hold measured while the view was still blending in (just switched to first
  person, aiming: the eye still behind them) had the chest 5-6 ahead of the eye, not 0: the arms came out at 19% of their
  size, same spot on the screen, until the hold was taken again. Holds are taken only all the way in first person now; the
  character's size only all the way relaxed (easing back from aiming, Peter's came out 0.80, not 0.98). The user liked the
  game's moves from there but not the size: `[Camera] FirstPersonArmFlat` (percent: the arms that much farther off at the
  same spot on the screen, shrunk toward the eye by as much more, so no nearer a wall - a flat perspective, no gun
  swung past a lens) and `FirstPersonArmSize` (how big they look: clip x, y zoomed about that spot, x' = m x + (1 - m) S w,
  m = size / flat; bob and swing as far on the screen). What the off hand holds in a reload is a model on it, up to 6 x
  the shrink from the gun hand - nearer than that taken for the gun's, or it floats unzoomed.
- **Third person's arms (`FirstPersonMoves=3`, 2026-11-05, tuned on Peter only)**: the game's pose of both arms left as it is,
  no aiming hold, the off hand free, on a steady chest - the relaxed hold, going to the aiming one by the game's own aim
  weight (the hand higher aiming when flat: a gun pointing ahead has no perspective drawing it up the view). On the move
  the arms go 0.8 of the way toward the relaxed carry (`fpv calm`): the rhythm kept, the run's gun on its side gone.
  Zoomed, the gun hand is kept on its aiming hold. The relaxed carry is saved after the aiming holds in fpv_holds.dat
  ("FPR1" section; older files have none) - until it's measured the gun stood at a tilt, uncalmed, for the first seconds.
  The hidden body sits below the screen in this mode: behind the eye, the shoulders stretched to it fanned out black
  over the bottom corner. Arm bones no nearer the eye than 0.4: at 0.22 Brian aiming wasn't drawn at all (0 skinned draws).
  The character's size is the highest eye seen relaxed: pressing to aim, the eye starts down before the stance does.
  Peter tuned further: aiming, the gun turned in (pitch 10, yaw 30, roll 10; straight ahead and flat, all that showed was its
  back end, filling the view) at arm size 70. The off arm hangs under the view: moved up as it is, its hand at (-0.5, -0.6) of
  the screen, turned so shoulder to hand runs at 118 degrees (the shoulder under the screen - in the corner its stretch
  showed black spikes), from the relaxed and aiming poses by the stance, let go by the hands' distance (a reload reaches the
  gun). The hidden body's place worked out through the zoom, under the middle of the screen. The worst pop was the zoom, not
  the stance: the gun in under the crosshair at the game's lens speed (time constant ~40 ms) moved 44 px of 320 in a frame;
  eased over 0.14 s, 5. The stance: the chest goes with the pose at once (eased, the aiming pose hung off the relaxed chest
  and swung wide), the gun held as shown and eased onto it, the zoom spot and the gun's turn eased.
- **Two-handed holds**: the game's relaxed carry has the off hand on the gun (Quagmire 0.44 from the gun hand, Lois
  0.36; Chris's bow string 0.42 - left out): that grip (against the gun hand) is where OffHand puts it, unless the game's
  moves take the gun hand 12+ degrees off its hold (a reload). Quagmire's aiming hold is how he zooms - the Tommy gun
  stood up by his face: his barrel (gun hand to grip) is turned to point at the crosshair 20 ahead. Zoomed in, the
  gun hand is kept on its hold (the game's raise to the eye not let through).
- Test copies: other sessions may use FGUY-Research / FGUY-Look at the same time (their stop_tests closes ours, their
  pad commands reached ours): first-person tests run in A:\Games\FGUY-FPV (same layout).
- One-handed guns (Brian, Meg, Lois, Stewie...): the game's aiming pose keeps the off hand at their side, and none of its
  moves (reload included) bring it nearer than ~0.9 (eye 1.8) to the gun hand - nothing of its own to copy. The off hand
  is bent onto the gun instead (the gun hand mirrored across the view, just left of it and under it) whenever the
  game's own pose has it out of the view; in the view (a reload's hand) the game's. Not for dual pistols (Carter,
  Cleveland: their second gun sits low, just out of the view - it'd be taken for a free hand).
- `tools/start_arena.ps1` (`-Enemies` keeps them), `tools/test_fpv_all.ps1` (every character's first person on a
  contact sheet - `tools/re/fpv_frames.py`).

## First person: Peter's poses, what the game says about its moves (2026-11-06)

- **The truth to match**: `fpv armsraw 1` (none of ours) + `fpv hide 0` + `fpv 0 1 1.3 -3.2 1.4 0` (the eye behind him) + `fpv fov 30`
  + `fpv face 0` (his body stays, the view turns): holding the right stick orbits the camera round the game's own Peter.
- **His grip isn't broken**: fingers are never touched by us. The game's fist on the ribbed grip is what first person showed,
  from along the barrel - the gun's drum in front of the hand. Peter's forearm runs in line with the barrel, so turning the
  arm to point the barrel ahead points the forearm at the eye. Turned as one (pitch, yaw, roll about the gun hand) with a roll
  of -40, the gun's side, the label and the forearm show.
- **A pose per stance** (`g_peterPose`, motion.cpp; `fpv pose <n> <right> <down> <pitch> <yaw> <roll> <off x> <off y> <off way>`):
  0 relaxed, 1 aiming, 2 zoomed, 3 reloading, 4 hands empty, 5 hands empty and not swinging. The relaxed carry has the barrel up
  (pitch +40) and aiming ahead (pitch -40) for the same look on the screen: firing no longer moves the gun across the view.
- **The upper body's block** ([player+0x13cc]): +0x60 the clip - 0 none, 0xb aiming, **4, 5, 6 the three parts of a reload**
  (matching 4 alone let go of the pose with the hand 0.63 off its hold, the gun under the screen for half a second);
  +0x5c what's under it - 0 standing, 2 walking, 4 running, 0x16 in the air. Landing: +0x5c is 0 again, but +0x104 (relaxed)
  stays ~0.06 for half a second - the arms are held until it's back.
- **The gun out**: [player+0xef58], the gun; 0 from the moment a melee swing starts until they aim or fire (it doesn't come back by
  itself). A swing is no upper-body action (+0x104 stays 1). Whether the gun was drawn wouldn't do: lowered out of the view the
  game stops drawing it, and it looked gone for good. Found with `fpv snap <0..3>` / `fpv diff 1`: what's the same in 0 and 2,
  the same in 1 and 3, and differs between them.
- **Reload's old drum**: let go at the gun - near the eye, the arm being shrunk - at full size: a black wall across the view.
  Left out within 1.2 of the eye while reloading (FirstPersonSkipDraw).
- **The black slabs by the sleeves** were a worn model's own outline (models.cpp): a fixed width, however small the bones - 14 times
  too wide on arms shrunk to 0.07. Not the game's outline shells: `fpv shells 0` changed nothing. Now by the bones' size.
- **Field of view**: a slider (60-120 by 5; 80 the game's, kept as 0). Its code in the input: opts bits 4..6, and the top bit in
  `aim`'s bit 7 (aim is 0..100) - the size of the input unchanged, the old five choices their old codes. Checked in a match at 115:
  every PC has it, aim sensitivity read right.
- **The arms' ink** (2026-11-06): the game's strokes are made for its camera, in view space, and a player's own are left out in first
  person, so the arms were flat colour: one pink shape, no fingers. Now each piece of the arms is drawn first in black, 2 px (at 720
  lines) off in eight directions and a little behind (depth bias 1e-5, slope 4), then as itself (cosmetics.cpp ArmInkDraw): a line round
  the arm and round a finger over the palm. More bias and the lines between the fingers go; none and the black covers the arm. Two traps:
  the black shader has to be the game's shader model (its VS are vs_3_0: a ps_2_0 draws nothing, no error) and characters draw with
  ALPHABLENDENABLE on (ONE, ZERO), so that says nothing about the pass; ZWRITEENABLE does. `fpv ink <px> <bias> <slope>`.
- **The idle fidget, third person's arms**: stood still it plays every few seconds, and only moving was held out in mode 3: the off arm
  thrown straight up, the gun swung 52-68 degrees, the off arm and most of the gun gone at its height. Held out now by its own weight.
  Tests of the first few seconds never see it: stand still 15 s.
- **Arms gone for a few frames of every punch**: the game drew none of the character (`fpv vmcount 1`: 155 skinned draws to 148, the
  player's 7 missing). A punch lunges them forward at 17 m/s (the eye 1.44 m in 5 frames) and the hidden body, 0.4 ahead, comes too near the
  camera: a frame's 0.28 m at 60 fps, half that at 120, so **720p at 120 fps never showed it - test at 1080p too**. Peter's hidden
  body is now far below (`fpv hidedown <y> <ahead>`, -12 0.4; others -3 0.4). Same punches, one launch, alternating, complete bursts: 33
  of 1500 frames with no arms at -3, none at -12. Farther ahead (1.2) worked too but what's stretched from the shoulders then lay over
  the sleeves. Ruled out on the way: moving it only a little within the view (-1.6: 1 and 12 frames against 15 and 13).
- **A punch, hands empty**: the gun arm is aimed like the off arm (`fpv punch <up> <in> <twist> <thumb>`, 35 25 12 -1: the right hand is the left's mirror, fingers -x, thumb -z, palm +y; +z turned the fist over, palm out) about the hand, its
  path the game's: it swung past the lens, on end and then lying across the view, and left a handless sleeve for half a second. Pushed
  ahead as the off arm is. The off arm's hold hands empty: where it's been lately (kOffFollow), plus what's between that and its
  look at rest with the gun - the game holds it elsewhere then, and it stayed high, hand up.
- **Peter's arm rig, rebuilt** (2026-11-06; what's above about poses, wrists and the off arm's push was for the flat rig this replaces).
  The tilt, the arm's visible end and the punch out of its socket had one cause: a third-person arm turned rigidly about the hand and
  drawn from afar (5 units, zoomed 3.5x). In flat perspective a gun pointing ahead shows only its back, so it had to lean; nothing tied the
  arm to a shoulder. Now (motion.cpp g_rig, ArmIk; `fpv rig <hands ahead> <shoulder right> <down> <ahead> <shrink> <kept ahead>`):
  - *True perspective*: hands 1.3 ahead of the eye at their own size. Not a person's arm's length: the arm is 0.58 (0.284 + 0.298) but the
    drill as long as his body - at 0.42 the view was inside it.
  - *Kept off the camera*: a bone near the camera and the game draws none of the character. What's drawn is shrunk 0.1 and put 1.2 ahead of
    the eye, the draw's eye moved up the same (hand[16], FollowX): the same on the screen, every bone well away from the camera, past
    kShellNear. The muzzle's flash, placed by the game, comes out near the crosshair.
  - *Two-bone IK* (ArmIk): the hand stays where the pose has it, the shoulder goes to its place (0.5 right, 0.5 down, 0.9 ahead: off the
    screen), nearer the hand if it wouldn't reach - nothing stretches - and the elbow bends down and out. Forearm and upper arm keep the
    twist the game's pose has to the hand. Hands empty (and the off hand) lined up with the forearm.
  - *The gun aimed by its own axes*: g_gunInHand (measured: `fpv vmlog 1`, "gun in hand"; its +z the barrel). Each frame the stance's hold,
    between relaxed and aiming as the game eases, is turned to the pose's pitch, yaw, roll from straight ahead; all the game does from the
    hold comes through on top (a reload at 55%, a punch at 70%). Relaxed and aiming are the same pose: firing doesn't move the gun.
  - FirstPersonArmSize / ArmFlat no longer apply to Peter. A worn model's parts partly on the hidden body aren't drawn with the arms
    (Part.stretched: the vest's side made a brown slab along the forearm).
  - *Fingers* (HandShape, g_fingers; `fpv fingers <relaxed> <tip> <grip> <tip> <step> <fan>`): the game's hands are made to be seen small -
    each relaxed finger curled its own way, on the grip the middle and ring at 50 degrees and the first straight. Posed by bone now (the
    hand, then a finger's two bones three times, the thumb's two, the palm). Alike and parallel they closed into a mitten: each finger
    curls 12 degrees more than the last and they fan 0.2. A punch keeps the game's fist, which is better than one posed.
    The grip is thicker than his fingers go round: even the game's own hand has two tips out of its far side and the thumb through it
    into the gun, unseen on a small character. Curled 35 + 25 the fingers lie round its front edge, the thumb along its outside
    (g_gripThumb, `fpv thumb`). Look at it from all sides: `fpv rig 0.8 0.5 0.5 0.4`, then `fpv pose 0` with yaw 75, -60 and pitch 55.
  - Ink at this rig: bias 3e-5, slope 3 (at 1e-5 thin dashes ran along the forearm).
  - *A reload*: 90% of the game's swing taken out of the gun (`fpv less <reload> <walk>`): it swung over the crosshair. The off hand moves by
    what the gun arm's moved by, so it keeps its work on the gun: the old drum off, the new one seated. Its shoulder then goes under the hand,
    below the view: reaching across from its own side it wouldn't reach, and the shoulder was drawn into the view, a hump of sleeve.
  - *The walk*: 70% of the game's swing taken out of the off hand, by st.moving: it went off the bottom of the view.
  - *Aiming, the off arm*: kept as it is relaxed (rot, pos and root toward its relaxed hold, with its look at rest), unless reloading or
    throwing: the game's aiming arm is elsewhere and it jumped up at every shot. Its "reaching for the gun" is off then too.
  - *Clips*: firing and zooming are action 4 too (clips 11, 2); a reload 4, 5, 6; a power-up used or a weapon changed 7..10 - their swing of
    the gun taken down 80% (st.actionNow). Going by the action alone it counted every shot.
  - The muzzle's flash needed nothing: the barrel points at the crosshair, so the flash, placed by the game near the middle, is at its tip.
  - The swing trail is left as the game's: it shows the swing, behind the arm.
  - Rough: the gun sits lower running. The first second of a reload, the off hand is below the view.
- **From a recording of a real match** (2026-11-06; the empty arena hides most of this - test with enemies, `start_arena.ps1 -Enemies`):
  - *Field of view*: set outright, +0x88 killed zoom at every value but the game's: zooming narrows that very setting. Now scaled by the slider's
    share of 80. All 13 values checked hip and zoomed: the arms the same, the world and zoom right.
  - *Shots*: measured, pad, pulls against rounds, first and third person alike: all fire at 250 ms apart or more, half at 150 - the drill's rate.
    An empty clip doesn't reload by itself ([R] RELOAD). A pull right after a punch or in a taunt fires. Count rounds with +0x4ffc.
  - *Recoil* (g_kick, `fpv kick`): each round (the clip going down) throws the gun back and up, alternately aside, 12 ms in, 85 ms back. Zoomed
    the game's own is held out. Measured zoomed: 9% of the screen across, 4% up, 28% bigger at its height.
  - *No relaxed hold yet* (new players; each new character): it's taken standing steady and looking level, 34 s into a first game. Till then
    the gun's held as aiming. (The game's relaxed hand stood in at first: on another chest, the gun huge and turned.) The off arm's out of view meanwhile.
  - *Muzzle flash*: placed by the game at the real muzzle and sized for far away, a third of the screen. It isn't one of the models about the
    gun (`vm` draws are the gun's). The rig kept 1.2 ahead, not 0.7, for a smaller one - real distance is all that sizes it.
  - *Outline shells*: the arms' and the gun's left out (FirstPersonShellInner, FirstPersonInked): ink instead, the gun's too. Their damage flash is the game's: whole arms solid red.
  - *The black flashes* (arms and gun near black for a moment on shooting, a black hexagon at the flash; the emblems' glow gone): aimed shots only.
    The aimed view's final pass is its own shader, with depth of field (BloomSampler, BlurryScreenSampler, DepthSampler, ScreenSampler, focusPoints;
    the hip's has no depth). For the first 4 frames of a shot's flash the picture came out without the arms, the gun and the flash's core - the
    flat "dark fill" was the wall behind, inside their outlines - and without glow. The draws are all there, 1 projected shadow and 2 of the flash
    more than the frames after; focusPoints, HDRParams and the threshold don't change (666.667 -1.33333 -25 3; 0.4 0.5). Holding focusPoints' z, w at 0 (near
    blur off) ends it: Hook_SetPSConstF does that in first person. One launch, alternating, 300 frames each: off 5 of 5 shots dark, 4 frames each
    (`fpv nearblur 1`); on none, at the spawn and at a wall. Why only under a flash isn't known. Hip fire had looked clean for days: test aimed too.
    Tested on the Space Station only. On the way: the video repeats frames (3 of about 21 shots caught). Left out with FGUYMOD_TEST_PSSKIP, still dark:
    the projected shadow; the flash's own shader (2 dark frames of 4 left). `fpv framelog <n>` (with FGUYMOD_TEST_PSTRACE=1): each frame's
    shaders in order; `FGUYMOD_TEST_PSCONST` values are logged with it. tools/start_arena.ps1 -Map 1: the Space Station, 2: the Airport.
  - *In the air the off hand turned to the side*: there the arm's put on its aiming hold, and its look kept for rest and for relaxed was missing: now
    kept = max(relaxed, calm). kAirLess .85: less of the game's flung arms in the air.
    *Still turning after a while in the air* (user: "for a long time", the Space Station): `+0x5c` isn't 0x16 for the whole jump. Standing 0x16, on the move
    0x17; after 3/4 s the game goes on to its fall, 0x14 and 0x15. Only 0x16 counted as the air, so the arms left the view in the fall and the off hand
    came back up on landing. Now 0x14..0x17. kAirEase .03, not .08: at takeoff the off hand flicked up before the air had eased in. Tested with
    `teleport` 45 m up and standing and running jumps, every frame. Left open: in that 45 m fall the arms go for about 10 frames just as the view
    drops past a floor plate at 25+ m/s, near the landing; not seen in the jumps. Why isn't known.
  - *Depth of field*: [Graphics] DepthOfField, 0 by default, Main Menu > OPTIONS > CAMERA (the GRAPHICS page is full: 6 rows of 100 and the footer). Off: all of
    focusPoints 0, in both views. On: the game's far blur, the near half still 0 in first person. Aimed at the spawn, edge strength (mean of neighbouring
    pixels' difference) 5.3 off, 3.0 on in first person, 4.8 and 2.8 in third. Black flashes again, one launch, alternating (burstflat's dark list; its "flashes" count stays 0 at
    this size, the dark list is what counts): the game's near blur 5 of 5 shots dark twice, the fix none twice, off and on alike, 48 aimed rounds.
  - *Left and right mouse together, shots missed* (user, keyboard and mouse): not reproduced with the pad - every combination 10 of 10 in both views, about
    4 rounds a second hip and aimed. [Debug] ShotLog=1 logs both mouse buttons and each round, for a real session. Its first version logged no clicks at all: the
    buttons aren't in the key table but the raw poll's 0xaefae8 (bit 1 left, 2 right); it read the keys' previous half. Its motion half was never switched on either.
    `kbm offline` puts player 1 on the keyboard / mouse controller in a pad-started match (device type 8, keyboard mode on), `kbm buttons <bits>` feeds the raw poll
    with nothing really pressed: rounds the same in both views for tapping left, left then right, both together, left with right held (6 of 6 each). Held left fired
    1 round in both views, with `winkey 256` too, while the user's own held bursts run at 0.217 s: the feed doesn't cover how a real held press reads, so it says nothing about
    holding. Real clicks (mouse_event) only with the user away from the PC: the test copy has to be in front, and they were using it.
    A real session, first person on the Space Station, left and right clicked in turn for 5 s: 23 left presses, 20 rounds; the 3 without a round came after the clip
    reached 0 (20 rounds gone in 4.4 s, then the reload). Each round about 75 ms after its press, 0.19..0.31 s apart (mean 0.23). No missed press with ammo in it. Steady frames went
    from 120 to 60 fps in it; the rounds' spacing didn't change. Walking speed, pad, 1.2 s, two tries each: walking 7.1 m in both views, firing 7.0, aiming 4.5 (firing or not):
    aiming costs over a third of the speed, in both views alike.
    *Found* (the user: spam both for 5 minutes, a controller does it too): steady frames. `spam 0 300 100 10` (fire and aim in turn, each press logged) with the reserve (player +0x5000) topped
    up, d3d9 as the user has it: 60 fps fell to 30 at about 95 s on a moment's hitch and stayed (20 at worst), 100 rounds in 30 s became 85-90, and presses with ammo in the gun fired nothing
    (9 first person, 30 third, run after it in the same match at the lowest rates: a press before the cooldown's end is lost, and at 30 fps it ends later). With steady frames off the same spam held 83 fps
    for 4 minutes, GPU 63%, Firefox 5%: nothing builds up. The rules for steps above the refresh were used below it too: 4 late frames of 60 went down; back up needed 95% inside 70% of 16.7 ms, and 12 ms
    frames never are. Now under the refresh: 15 of 60 down, 90% up. Again, the same run: first person 60 fps throughout, no step, 977 rounds of 918, 6 such presses; third 978 of 871, 2, its two steps to
    30 back within seconds. What's left are reloads: from 0 rounds (clips 4, 5) and the end of one (clip 6, the clip already full), the same in both views.
    *And strafing* (the user: "when you walk it still doesn't shoot"): a real log, 18 rounds in the clip, 7 presses in a second, nothing, the upper body on clip 11 all the while. `spam 0 30 100 100 om` (their rhythm: left down, right up 50 ms on, left up, right down; on the mouse) with `kbm offline` and winkey's WASD: A, D, W+A, S+D fired 1-3 rounds of 99 in first person, 87 in third. W, S and standing the same in both. Stop-start 94 and 90 of 149 in both: the game's. Steady stick or W on a pad fired all 204.
    FaceView was it: on the move the game turns them toward where they go, and fires once they've got there; heading and +0x38 set to the view every frame, they never did (`fpv face 0`: 87, the arms swung off the view). Either alone holds fire back; +0x5c alone doesn't, but the arms hang on +0x38. Now +0x38 is turned for the drawing only: at the pose build (FirstPersonPoseStart, after the frame's logic - turned at the camera update it's still too early, the fire check comes after), put back at the next tick's input poll (FirstPersonUnturn). And for the camera update alone they're turned all the way and put back (FaceForCamera): the game's camera swings behind a turning body, and the view went round by itself. Strafing, 14 s: 61-63 rounds of 70 (53-55 with a reload in it), standing 62-64; the view's own turn 0.0-0.4 degrees over six runs (`fpv faceparts <bits>`: 1 +0x38, 2 +0x5c, 4 the heading).
    Left over: strafing, the flash, tracer and shell come from the gun where the game has it, off the drawn one.
  - *Online* (two copies through a local lobby, no bots): sync-check mismatches come only in the lockstep part while the level loads (ticks 150, 540..840) and are harmless: Deathmatch and story co-op both let go of lockstep about 120 ticks after everyone's level is up (tick ~807-867), and from there the logs read 0.00 off on average, 0.00 at most, 0 teleports. Seen in the released 0.9.8 too (one co-op match in 4, ticks 690-780). What parts first is the random numbers (the count left before the refill differs by 1, then 8, as the level comes up). Rates by small samples misled here: Deathmatch, Controller Look on, 5 of 7 matches with a mismatch; its CameraEase off 2 of 9; the mod off 2 of 10; 0.9.8 0 of 6 Deathmatch. Co-op: this build 0 of 4, 0.9.8 1 of 4. First person with strafing in a match: free-running stayed at 0.00 off. A mismatch before tick ~870 says nothing about the match: look at the `free - player` lines after it.
  - *The black sky* (user's video, the night neighbourhood, 1.5 s at each reload): the sky dome is a draw centred on the camera, and was told by "no rotation, full size"; on some maps it's a tenth of that size, so a reload left it out with the models the off hand holds (hand[4..6] at 0 from it). Now told by its place (c4..c6) being the camera's (where c0..c3 have the eye: DrawAtEye). Maps 4 and 6 of Multiverse Madness: about 100 frames black in 420, none after; none on the other five. Space Station's sky is black, so it never showed there. The 3-4 dark frames at a taunt's start were something else: a worn model (the Adventurer's hat) drawn by models.cpp, which the view-through-the-head hiding didn't reach (FirstPersonPullHiding; kPullHide 0.8). All its pieces, or bits of the body flicker.
  - *The hands jumping for a frame* (user's video, an online match on 0.9.9's build: 49 pops in 517 distinct frames, about 1 in 8, the view still or not; arms stretched up, a fist with no gun, the gun gone or elsewhere). Not reproduced here, offline or online, at any turn speed (the same detector, tools: scratch popcount, 0 in 16 runs either side of the strafe fix). The older recordings are gone, so nothing to compare with. Suspect, not proven: turning +0x38 at the pose and putting it back at the next tick's input poll - the render thread could read it either way. Done with: nothing the render thread reads is changed for first person any more. No turn of the arms is needed: with the body 90 degrees from the view they come out right as they are (a turn of them at drawing, either way, put them out of the view). While the camera updates only the heading is set and put back (`fpv faceparts`, 4; the view's own turn 0.0-1.0 degrees over 7 strafes). Strafing the game's sideways run turned the gun with the chest: its arms held to the relaxed carry by view.turned (all of it from 0.5 rad). Firing strafing 61-62 of 70, as standing; three matches online 0.00 off. The zoomed pose's off hand raised (-0.6, -0.72), in the view while aiming.
  - *The hands jumping for a frame: found* (2026-11-09). Not the body's turn (+0x38): that suspicion was wrong, the rate didn't change. Reproduced only at the user's conditions: 1920x1200 and ~55-60 fps, recorded from outside the game (ffmpeg gdigrab `hwnd=`). The `shots` burst captures inside the game hide it (they stall the render thread: 0 pops in 12 runs while the recording of the same scene had 78). Cause: FollowX moves the arms by camera x pose-camera^-1, the pose-camera taken from the newest pose, but a frame draws the pose it was skinned with, 0 or 1 older (posing and drawing drift: 130 of 720 frames saw no new pose, as many saw two). The arms are drawn 0.13 from the eye (shrunk 0.1 about it), so the few cm or degrees between two poses threw them across the view, bigger and off-screen or small and central. Measured: the gun against the pose's eye off by over 5 cm on 57 of 720 frames with the pose taken, none with the best pose. Fix: hook 0x4b02d0 (skins each piece; `instance`+0xb0 bones, +0xb4 palette, 12 floats a bone, the character's own space). The hidden body's bones are in it, scale 0.001, at hideAt: that names the pose. A ring of the last 8 poses (motion.cpp `ring`), matched on it; poses that tie (walking straight, hideAt doesn't change) told apart by the character's place, c4..c6 of the arms' first draw. FrameArms is now taken at the arms' own first draw, not at the first world draw (the check by the gun hand uses the newest). Fix on/off in the same build (`fpv armmatch 0|1`), strafing: 21 pops against 0, 0; walking 0, fast turns 0 (7 flagged, gun in place: the detector's), firing while strafing 0, Adventurer skin 0. Research: `fpv framediag <n>`, `fpv posediag <n>`, `fpv skinlog <n>`.
  - *Jumping and landing* (2026-11-09): with the game's jump held out of the arms (kAirLess, `calmed`) the gun moved 0.1-0.4% of the screen through a jump and a landing, stiff. A spring of their own (motion.cpp kJump*, Peter's rig only): on the rising edge of clip 0x16/0x17 a kick up, on leaving the air (0x14..0x17, over 0.12 s in it) a kick down by the time in the air, falling a small float up; 11 rad/s, damping 0.75. Measured (`fpv jumplog <n>`): +5% of half the screen taking off, -7% landing, settled in about 0.4 s. It goes into st.offset (so frame-time smoothing still applies), the off hand through leftShift, the gun tilted by it (pp.pitch), 30% of it zoomed in.
  - *Stiff arms after a landing* (2026-11-09, user's video): the landing hold (`landing`, kept while the relaxed stance `rest` < 0.5) kept itself on: running after a landing rest stays 0.11, so airNow stayed 1 and the arms were held until they stopped (4.9 s of 4.9 s on the ground). Now at most g_landHold, 0.45 s after leaving the air (`fpv landhold <s>`; measured 0.5 s). *Gun huge for a frame or two, aiming on the move*: in the user's log clip 10 (always action 4, aim 1.00) lines up in time with those frames: the game's clip has the gun pulled in, and the ease on tossing (clips 7..10) takes a few frames to hold it out. Clip 10 is the gun animation on the 1 key (about a second; key 49 in tests): firing cuts it short, leaving only its first frames. Holding it out outright took the animation away, so it's faded in over kSettleIn, 0.25 s, at aim > 0.9 (`fpv clip10 0` off). Faded only within 0.35 s of a shot (the user: pressed by itself it didn't feel the same; its snap is the animation). Before that change, standing: 5 pops in 5 presses off, 0 on, the animation plays; firing and pressing 1 every 0.3 s: 47 off, 9 on (8 of them the animation's own pose, 1 a real one-frame jump, gun empty). Test tools: `spam ... m` and `kbm buttons` work offline only; online a keyboard player's buttons are `kbm click <ms>` (left) and `kbm rclick <ms>` (right). Count rounds ([Debug] ShotLog, `fpv shotwatch 1`): a firing test with no rounds tested nothing.
  - *The 1 key at full strength* (2026-11-09): clip 10 was among the power-up clips (7..10), held to a fifth (kActionLess) ever since the arms were rebuilt on 11-06; the user missed the animation as it was. Clips 7..9 now (`fpv clip10full 0` back). Recorded: the gun swings up and forward and comes back, about 0.4 s. Firing and pressing 1 every 0.3 s: 57 flagged jumps (20 before), the fade after a shot only covering the first 0.25 s; real shots alone 0 of 37.
  - *Two thin dark lines on the ground, looking down, gun away or not* (2026-11-09, user's video; reproduced: F for a melee swing, look down at the road, an outside recording). A wedge that swings with the view. NOT the put-away gun (not drawing it changed nothing), NOT the shadow maps (leaving the player out of every 512 x 512 or smaller target changed nothing). Not found yet; the game's projected shadow is the next thing to try.
  - *The 1 key zoomed* (2026-11-10): zoomed in, `pass` held the gun on its hold, clip 10 too - nothing happened. Let through (`zoomClipNow`). The 1 and 2 keys pick the first and second weapon; with one weapon, 1 just plays the draw (clip 10).
  - *The off arm alive while aimed* (2026-11-10): its place is fixed on the screen, cancelling the gun's sway and bob. Now the sway, a stride of its own (`kOffStride`), a breath, and a spring a shot pushes (`g_offLive`, `fpv offlive <i> <v>`). Recorded: a shot about 10 px up and back, aimed walking about 8 px.
  - *Peter's weapons* (2026-11-10): the wheel's ids 20 Nail Gun `00000004:0000014a`, 21 SMG `00002377:000000b0`, 22 double rocket launcher `00000009:0000014a`, 23 Chaingun `00000020:0000014a`, 24 Power Driller `00000152:0000014a`, 25 saw blade thrower `00000158:0000014a`. The weapon in hand is `[player+0x2240]`: asset id at +0x18, clip at +0x164, reserve +0x168, clip size +0x170. `player+0x4ffc` is only the clip of the object at `[player+0xf0]` - stale after a respawn or with a second weapon, so shots went unseen. Each weapon now has its own holds (`WeaponHold`), learned when it's first aimed; every weapon but the first is carried as it's aimed (`aimCarry`: the game's carry has them low or turned round, the off hand in the middle of the view). All aim within about 20 degrees of the nail gun; just after the draw some are held 140 degrees off with the stance still saying aiming, so a far-off hold is refused (`aimPlausible`). `fpv aimcarry 0`, `fpv weaponholds 0`, `fpv weaponlog <n>`.
  - *Testing weapons*: `FGUYMOD_TEST_ALLWEAPONS=1`, pipe `wpn 0 <id>` buys (on the open wheel: ` once), `wpnsel 0 <id>` picks as the mouse does. The ` key toggles the wheel and a pick leaves it toggled: one more tap before the next. In the match WASD walks, the wheel is the mouse's. The chaingun needs the trigger held to spin up, short clicks fire nothing. `hurt` no longer needs an earlier hit to copy.
  - *The two-handed weapons* (2026-11-10, the user: "the big weapons hold wrong, look at how they do in 3rd person"). Third person, seen from the front (the camera orbits a player standing still: `kbm mouse 40 0 15` is half a turn): the rocket launcher, the chaingun and the Power Driller are carried at the hip, the gun hand on the rear grip, the off hand on the handle 0.85 farther along the top (one animation set for the three); the draw brings it from behind the back up over the head and down; the launcher's reload tips it up, the driller's swings it aside with the off hand let go, the chaingun has none. The Nail Gun, the SMG and the saw blade thrower are one-handed (the saw rests on the shoulder like the Nail Gun; its reload drops the battery). First person: a `WeaponLook` per two-handed weapon (`g_weaponLooks`) - the off arm goes with the gun arm as one piece (`with` = 1, its bones the game's, the stance easing on both), the reload at full size, the draw at 40% (`g_drawLess`: whole, both arms stood in the middle of the view), its own pose (held 0.82 right 1.05 down, pitch -11 yaw 24 roll 2; zoomed 0.55 1.0 -9 15 0), the gun hand 1.1 ahead, each arm solved from a shoulder set off its hand (`rig`), the gun hand's fingers the game's. `fpv wpose`, `fpv wrig`, `fpv drawless`. The number keys 1..6 pick the six weapons.
  - *A body turned from the view spoils a hold* (2026-11-10): at a spawn the body faces 180 degrees from the camera until they move or aim (`view.turned` 3.14), and the draw (clip 10) says aiming 1.00 without turning it. The chest in the view (`cq`) isn't made up for the turn, so an aiming hold taken then was 140-180 degrees round - the Nail Gun's own too, after pressing 1 standing at a spawn: every weapon set against it came out backwards. What looked like "held another way for a moment after the draw" and "the saw blade thrower turned backwards at rest" was this. An aiming hold is now taken only in the aiming clip (11), 0.25 s into it, the body within 0.15 of the view, within 40 degrees of the first weapon's kept hold (`aimPlausible`; the first weapon's aiming hand read once from fpv_holds.dat, not from the session's hold).
  - *Weapons' holds in fpv_holds.dat* (2026-11-10): key = the character's + (the weapon id's low 15 bits << 16) (`WeaponKey`); read when the weapon first comes to hand (so its draw is right the first time), written when one is taken standing still, level, not zoomed. The five in dist were taken so (`tools`: scratch `holds_tool.py list|strip|merge`; `fpv holdslearn 1` takes them afresh): select, one shot, stand 3 s. The updater overwrites the file, so everyone gets them.
  - *Testing while the user is at the PC*: gdigrab records the screen where the window is - with their browser in front it recorded the browser (deleted). Then the game's own frames: pipe `shots <n <= 600> <folder>` (scratch `rec2.ps1`). A match started behind their windows stalled once at load, and a guest copy was closed once.
  - *The two-handed weapons' off arm* (2026-11-10, the user's screenshot: "the left arm is totally cooked" - a sleeve a third of the screen with a stump on the launcher). Measured in the game's pose, the same in third person (`Diag: any view`, `Diag: game arms` under `fpv weaponlog`): the hands are 0.83 apart, the gun hand on the black stick at the rear, the off hand on the front post of the handle frame; the off arm's forearm lies across the belly from the left (elbow 0.50 left and 0.11 above the hand, shoulder 0.70 left 0.20 up 0.39 back of it), and the game stretches it to get there - its bones' translations 1.7 times the gun arm's (21..24: 0.25 0.16 0.24 0.11 against 0.147 0.093 0.130 0.089), every scale 1. The hand's fingers point along the weapon, palm down: the wrist is bent a right angle in the game too. Solved from a shoulder set near the hand, the forearm came from behind and under, the hand hidden beyond its end, the sleeve (as wide as the arm is long) filling the view. Now the elbow is set from the hand as third person has it (`rig[4..6]`: 0.5 left, 0.1 down, 0.05 back), the forearm's bones stretched from there to the hand the game's way, and the upper arm goes down and back from the elbow (`g_offUpper`) as long as it takes for the sleeve's end to be under the view's edge: a sleeve up from the bottom, an elbow, a bare forearm across to the handle. The weapon lower for it: held 0.86 right 1.25 down, pitch -9 yaw 22 roll 4; zoomed 0.6 1.2 -8 15 2. `ArmIk` takes the hand's place (a stretch moves it). `fpv offupper <left> <up> <ahead> <under>`.
  - *The two-handed weapons' off hand: a grip of its own* (2026-11-10, the user's video: "the way the hand is just looks so weird and it doesnt feel like its even holding the weapon ... there's some stretching going on"). Each of the three has an arch of a handle across its top; the game's off hand lies on the arch from behind, fingers along the weapon hooked over it, palm down. From the eye that's the back of a mitt at the end of a forearm with the arch hidden under it - a stump, at any angle of the forearm (tried: from the left, from below, from the corner). Fingers only show when they lie across the line of sight. So in the view the hand is across the weapon's top just behind the arch (`grip`: 0.2 left, 0.14 down, 0.2 back of the game's place, in the weapon's axes; fingers to the right tipped 40 degrees down, curled 45 and 80 at the tips with `HandShape`): a fist with its fingers showing, on the weapon, the arch standing in front of it. Only while the game has the hand at its hold (within 0.06, gone by 0.2 off it): through a reload or the draw it's the game's hand, open. The stretching seen was the upper arm pulled up to an elbow in the middle of the view - the sleeve smeared; the elbow is now 0.45 left of the hand and 0.2 above the view's bottom edge (`rig[4..6]`), the upper arm its own length on down out of the view (the cuff shows), the forearm stretched from there (about 1.3 times the game's). `fpv wgrip`, `fpv gripon 0`. Recorded from outside (the test copy in front): launcher, driller, chaingun through draw, shot, reload, strafe - the arm's step between frames 0.02 px at rest, the only spikes the shots' kick.
  - *Arms gone and the gun big at the side, for a frame* (2026-11-10, the user's video: ten times in 24 s, walking and turning with the rocket launcher). The frame's arms were put for the wrong pose's eye and way ahead. Poses are told apart by where their hidden body is in the palette (`NoteSkinned`) - in the character's own space; turning with the body going round with the view, or running straight, that's the same for every pose, all eight in the ring fit (`ties` up to 8), and the tie went to the newest, or by the character's place, which doesn't change turning on the spot. On a frame drawn a pose behind (0 or 2 poses a frame: 2-6% of frames at 85-110 fps, more while something records the screen) the pose taken was a pose's turn off the one drawn - 4 degrees at a brisk turn - and the arms are kept 1.2 ahead of the eye and drawn from there 0.13 away (`g_rig[5]`), so 1.2 x sin 4 = 0.08 beside a hand 0.13 off: 33 degrees across the view. A run's step (0.06) the same way: twice the size, down out of the view. Fix (`g_poseByDraw`, `fpv posebydraw 0` back): each pose gives its eye and way ahead in the character's own space too (the snapshot's floats 26..31; the array is 34 now), and the frame puts them through the character's own draw's world matrix (c4..c6 of its first skinned draw: rows = the entity's axes as columns, its place the 4th) - right whichever of the like poses it was, and wherever the character's drawn. Kept for the frame's other passes (`frameW`). Also: nothing but the character's own draw keeps a pose for the frame now (`g_snapFix`; a prop within 3 of the feet drawn first would have - never seen to happen: characters are drawn first). Measured through window capture at 60 fps, fast turns while running: off 4 glitches in 9.5 s and 2 in another 9.5 s, on 0 in 3 runs and 0 in two 45 s wanders (the pose taken was up to 4.00 degrees off in 96 of 800 frames - put right).
  - *Tests never had the user's field of view*: `FGUYMOD_FPV_FOV` does nothing - the setting is read from the ini only ([Camera] FirstPersonFOV; online it goes with the input). Every test until now ran at the game's own. The scratch `live_solo.ps1` writes the user's [Camera] and [Graphics] lines into the test copy's ini (restored after).
  - *Recording a test copy behind other windows*: ffmpeg's `gfxcapture` (Windows' window capture, what Snipping Tool uses): `-f lavfi -i "gfxcapture=hwnd=<n>:max_framerate=60:capture_cursor=0,hwdownload,format=bgra,crop=trunc(iw/2)*2:trunc(ih/2)*2:0:0"`. `gdigrab hwnd=` records the screen where the window is - whatever's in front. The game's own `shots` and every per-frame number logged (`fpv framediag`) showed nothing wrong in frames that were wrong on the screen: only a recording from outside tells. Scratch: `rec3.ps1`, `glitchscan.py` (the arm leaps or goes and is back within 12 frames), `live_solo.ps1` (one player through a private lobby, 85-110 fps as the user has).
  - *Seen once, not found again*: the Nail Gun black for single frames (5 in 590, strafing and firing by a fence, the game's own frames) - the outline shell's inside over it. 0 in seven more runs with the new code, 1 frame in one of three with the old.
  - *Hands empty: fists* (2026-11-10, the user's video: a forearm standing in the view during punches, the right hand gone after them, the left low over the gauge). Peter's punch is a haymaker - the gun hand from where it hangs (-0.78 -0.91 -0.35 from the eye: left, up, ahead, for an eye 1.8 up) snaps in one pose to beside the head (-0.10 -0.28 -0.53), goes round to its reach (0.54 -0.72 1.51) in 0.12 s, is held across the body (0.90 -0.90 0.63) 0.2 s and is back by 0.5 s; the body lunges about 2 m; the clip stays 0 and the gun stays away (`gunOut` 0) until they aim or fire. None of that is in the view from the eye. Now (`g_fist`, `fpv fist <16 values>`, `fpv fists 0` the old way): both fists held up in the view, a guard - 0.5 of the half width out, 0.62 of the half height down, 1.0 ahead - and each moved from there by what the game's hand does from where the game carries it, a quarter to a third of it (the off hand's a fifth), eased over 0.03 s (the snap). The hands are taken in the body's own axes (strafing, the body's a quarter turn from the view), against their place averaged over 0.4 s - on a run the gun hand swings about a place 0.6 ahead of where it hangs - but not through a punch (the gun hand above -0.45, beyond 0.9 ahead or inside -0.25: frozen, and 0.25 s after). The arms: shoulders under the corners (0.55 out, 0.62 down, 0.72 ahead: his arm's only 0.29 + 0.30 long, a shoulder farther from the fist than that is drawn up after it), elbows out and down; thumbs up, fingers curled 75 + 85. Switched all at once when the gun goes (`gunOut`), the fists coming up from under the view over 0.06 s; the gun drawn again comes up as it did.
  - *An arm ten times its length for a few poses*: `FromTo` (the least turn from one way to another) was built with 1 / (1 + cosine): for ways within a degree or two of opposite, a hair off unit length, the result scaled as well as turned. `ArmIk` turns each bone from the way it points in the pose it's given - and the hands-empty pose it was given has the arm hanging down and back, opposite the fists: 0.6 s after stopping a run, and at other eases of the old pose, the forearm came out 0.31 -> 3.14 long, a fist filling the view for a third of a second. (Likely the forearm standing in the view in the user's video too.) `FromTo` is two half turns now - about the first way, then about the way halfway between: the same turn, steady. And the fists' arms aren't solved from the gun's pose at all: elbow and wrist bent as the game has them this pose, the whole arm turned at the shoulder till the fist is as wanted, then solved - what's left to turn is the difference of two bends. The bones' own places are put back too (a two-handed weapon's off forearm is drawn out 1.7 times to its handle).
  - *Testing melee*: F is the punch (`kbm key 70 90`); three presses 0.45 s apart give two punches. `fpv weaponlog <n>` logs `Diag: fists` (the game's hands from the eye, where they're carried) and `Diag: fist <0|1>` (where each fist was sent and where its hand, elbow and shoulder ended, the arm's two lengths). Scratch: `skinscan.py` (how much of each frame is skin: an arm filling the view, or leaping for a frame or two - checked against the recording with the bug; an explosion's debris reads as skin too), `sheet.ps1` (frames of a recording side by side).
  - *The rig by character* (2026-11-10): Peter's poses, rig, gun axes and hidden-body depth moved out of globals into `g_charRigs` (motion.cpp `CharRig`, by skeleton size: 59 Peter, 50 Lois); `posed` is any character with a row. The research commands (`fpv pose`, `fpv rig`, `fpv hidedown`) change the row of whoever player 0 is playing; `fpv wpose|wrig|wgrip 9 ...` change that character's first weapon's look. Flags per row: `fingers` (Peter's shaping), `fists`, `weapons` (a hold per weapon), `plainOff`.
  - *Lois, first pass* (2026-11-10; test as her: `FGUYMOD_TEST_CHAR=Lois`, scratch `live_solo.ps1 -Char Lois`). 50 bones, hands 31 and 16, chest 5, head 7. Her first weapon is held in both hands (the off hand on its drum, 0.36 from the gun hand) and runs with it across the chest; aiming it's at the shoulder; a reload drops the drum; melee is an iron in each hand, swung with steam, and carried at her sides after until she aims or fires. On the old flat rig all of that was two crossed sleeves in the corner. On the new one: the gun and both arms as one piece, as the game has them, the game's moves at full size (Peter's two-handed path) - the gun hand at 0.66 right, 0.9 down, 1.0 ahead, the gun straight ahead (-1, 2, 0), under the crosshair zoomed (0.34, 0.8) - each arm solved from a shoulder of its own under the view (the look's rig: from each hand, 0.25 / 0.3 out, 0.35 down, 0.45 back), fingers the game's own (`plainOff`: no reshaped grip, no drawn-out forearm). Her gun's axes in the hand: `fpv vmlog 1` gave 0.0124 -0.0016 -0.9999 / 0.9999 0.0002 0.0124 / 0.0002 -1 0.0016 aiming (it shifts about 9 degrees in the hand between the carry and aiming). Straight ahead from the lower right a gun's line runs to the crosshair: it looks turned in, and isn't. The irons come through as the game plays them (no fists for her). Not done yet: her walk and run judged by the user, the irons' carry (the off one low over the gauge), her other weapons.
  - *A gun drawn before its character* (Lois's): the frame's pose is only known at the character's own first skinned draw (the dropouts fix), so the gun's first pass was placed by the newest pose with no correction and its later passes by the frame's - on the move two guns, the first pass a black shape beside the second, the gun black most of a run. Each frame: gun (2 draws, no snapshot), 2 other models of hers, her body, the gun's shell (left out, inked), the gun and body again. The early pass is left out now (`fpv early 0` draws it; firstperson.cpp ViewmodelDraw, only for arms kept ahead of the eye): the later passes alone draw it whole, with its lines. Peter's guns come after his body: none of his are left out (counted: 0).
  - *Lois as the game has her* (2026-11-10, after the user: the left wrist wrong; the run, the 1 key and the melee not vanilla). `CharRig::asGame`: one fixed turn from her chest to the view - the one that points the gun ahead as she aims (the aiming chest and hand for every stance, `sHold`) - and everything else is the game's own, against her chest: nothing calmed toward the carry on the move or in the air, none of the draw, reload, swing or jump taken out (`less` 1), no bob or jump of the view's own, both arms one piece whatever she holds, no re-aimed empty hand. (`fpv asgame <calm> <less> <bob> <strafe calm>` brings the old way back by degrees; strafing is still calmed - the fourth is 0 for it.) What that shows: relaxed and running, the gun lowered across her to the lower right, its drum and muzzle in the view, rocking with her steps; the 1 key's twirl (third person: the gun dropped to her hip, flipped up at her side, back) - the gun out of the view for about nine frames, her off hand free, then in from the right; the draw from the irons is the same twirl; the melee an overhand smash with the off iron, its sleeve across a third of the view for a moment.
  - *Her chest's own turn* (Lois): she aims with a shoulder back - the chest 63 degrees round from where it is as she carries the irons, 24 from the gun's carry (`Diag: chest turned`, under `fpv weaponlog`). Hung on the aiming chest alone the irons were one at the eye and one far off. The chest's turn about the upright from where it is aiming (the twist of R[chest] against the aiming hold's chest) is left on it (`H = Y H0`; the gun's turn and its pivot are still worked from H0): aiming 0, the irons square, a swing's twist of the body comes through. The irons' anchor: 0.36 right, 0.35 down for the aiming hand's place (poses 4 and 5) - her chest under the middle of the view, both irons in it.
  - *Wrists* (Lois): each arm keeps the game's elbow and wrist; only the upper arm is solved, from the elbow down and back out of the view (`fromElbow`, the look's rig the way: -0.35 / 0.35 across, -1 down, -0.45 back). Solved from a shoulder under the view the forearm came up from below and the off hand lay 70-80 degrees sideways on the drum.
  - *Scanning frames whatever the colours*: scratch `spikescan.py` - each frame's lower 70% in grey blocks, a run of 1..3 frames against a blend of the frames either side; `events.py` lays the frames round each hit side by side. Checked against the black-gun recording (56 hits in 10 s) and Peter's two-frame forearm. It hits every quick move of the game's own as well (a shot's flash, a swing, the twirl): each hit has to be looked at. `skinscan.py` is Peter's skin only - Lois's arms at night aren't a colour that can be told from the map. Lois, 62 s of everything: 21 hits, all the game's own moves; no stretched limb, no black gun, no frame out of place.
  - *0.9.9, players' reports* (2026-11-10/11). **The crash leaving a level in first person** (DINPUT8.dll, 0xc0000005, fault offset 0x265a1): `EyeNow` reading `[v.player + 0x38]` - the player's kept from the camera's last update and read while a frame's drawn; Quit Game frees them and the next frame reads where they were. Found by building the v0.9.9 tag in a worktree with `/MAP` (the code round the offset the same as the release's), reproduced with the release's own dll in the test copy (offline level, `fpv 0 1`, pause, Quit Game, Yes: the same offset, every time). Fixed: `Listed()` - still among the game's players and their memory there - before each read of a view's player while drawing (EyeNow, OwnLinesShow, FirstPersonNoLines, Taunting). After it: no crash in the loader in 14 exits.
  - *Another crash leaving a level, not first person's* (solved below - it was the loader's): `CRASH at 0x428c9b`, the game's free (0x428c70: `mov eax, [block - 4]` - an aligned block's header - then the C runtime's free), on a thread of its own that ends by freeing a block whose pages are already gone. With first person 6 of 14 exits, without it 2 of 9: not told apart by that. Not a second free through 0x428c70 (research `FGUYMOD_TRACE_FREE=1` keeps the last two million frees, each with its caller and thread: the crashing free was the block's only one). Not tested: 0.9.8's loader, the game without the mod. The crash log lists other modules' addresses on the stack now as well as the game's. After a crash the graphics driver refuses new devices for minutes (`wait_gpu.ps1` gives up at 240 s): test loops are slow.
  - *0.9.9, new characters (a .fgmodel in place of the character)*: in first person nothing of them was drawn - a gun and nothing holding it. 0.9.9 left out, for the arms' draws, every part of a model with a twentieth of its vertices on the hidden body (made for a vest worn over Peter); a new character's arms are one part with its body. Now a copy of the indices without the triangles that have a corner on what's hidden (under 0.93 of the biggest vertex, while that's small: at 0.6 a sleeve's top was drawn out toward the body in spikes), made each frame there's something hidden (`ibArms`). Test: `modelload 0 build\research\StewieTux_Test.fgmodel` as Stewie. Also: the outline's width went by the model's own skin matrix (0.9.9: `kOutline * size`) - a model made in other units would have its outline as many times too wide; it goes by the pose's bone alone now. Not reproduced: the head gone seen from behind (a player's picture) - the test model keeps its head in third person; needs their pack. Stewie's taunt in first person shows no Stewie at all, with or without a model (not looked into).
  - *0.9.9, new characters: what broke them* (2026-11-11, with the player's own pack - Toad, in place of Stewie: 5 parts, 3256 vertices). `struct Part` got a fourth member in 0.9.9 (`stretched`) and went from 12 bytes to 16; `Parse` still copied the file's parts - 12 bytes each - into the array in one memcpy. Every part after the first was read from the wrong place: its first index, count and texture another part's numbers - the head gone (its count read as a texture number, under 3: not drawn), the eyes left hanging with slivers down to the neck, wrong textures. A model whose first part holds nearly all of it (the test Stewie tux) looked whole, which is why the first test model showed nothing. Read one part at a time now. Toad whole again from behind; Peter: Adventurer (the mod's own, 0.9.9 had it wrong too) whole. Lesson: a struct that's read straight from a file isn't to be grown - or is to be read field by field.
  - *The game closing on leaving a level - the "other crash", solved* (2026-11-11). It was ours: `FirstPersonPoseStart` (every pose built, the menu's characters' too) read `[v.player + 0x38]` of a player the level had freed - about one exit to the menu in three the memory was gone. With first person on or off (a view's player is kept either way), so 0.9.9 can close on anyone leaving a level. Why it took a day: the game sets an SE translator (`_set_se_translator(0x97d790)` in main, 0x97d7f0) - a fault in its loop becomes a C++ exception, caught in main, which shuts down and returns; `exit` then runs the static destructors, and those always crash (0x428c9b, the game's free reading a block's header: an allocator already gone - d3d9.cpp `QuitGame` knew). The crash log only said faults in the game's own code, so the one line logged was that last crash: it read as a block freed twice on a worker thread (the stack's MSVCR100 frames are `exit` and `doexit`, not a thread's start). Traced frees (`FGUYMOD_TRACE_FREE=1`: the game's free, the runtime's free, `_beginthreadex` with `FGUYMOD_TRACE_THREADS=1`) showed no earlier free; naming the MSVCR100 offsets from its export table showed `exit`; the tick (0x4b2910) and the loop (0x44a0a0) showed no way out but `_exit` on the window's quit flag ([0xb06c54]+0xa, set only by WM_CLOSE / WM_DESTROY - logged now, none came). Then: the log says faults outside the game's code too (`FAULT in <module>+offset`, with that module's and the game's addresses on the stack) - the first run named DINPUT8+0x2b336, the map `FirstPersonPoseStart`. Fixed: `Listed()` there, and a view's player is forgotten at the end of the frame once they're off the game's list. 13 exits after (7 in first person, 6 not): all back at the menu, no fault logged. **A game that "closes by itself" with only CRASH at 0x428c9b in the log: look for the FAULT line before it.**
  - *HD world textures* (2026-11-11, after a player's "rendering issues close up": first person puts the eye against walls, tables, cars, and the world's art - 64 to 512 across - is a blur there; the mod already runs 16x anisotropic, what's left is plain magnification). The HD characters' making (skinbuild.cpp `Upscale2x`: bilateral, Lanczos to twice the size, unsharp; DXT again) for every opaque colour texture bound (stages 0-2) on a draw that isn't a player's character - the level, props, the other characters ([Graphics] `HDWorldTextures`, on; Graphics page 1 row 6). cosmetics.cpp: textures by a hash now (`g_hdOf`: thousands of draws a frame - the characters' 96 were a search), one read a frame while fewer than four are being made, the workers at below-normal priority, each made texture kept on disk by its pixels' hash (`FGUYMod\cache\hd\<hash>.dds`, 1.5 GB at most; the characters' too). The game isn't large-address-aware (PE characteristics 0x103): 2 GB for everything, Direct3D's copy of each managed texture in it - so a budget, `HDWorldBudgetMB` 192, past which textures wait as the game has them. Measured, the frat row map as Stewie: 51 textures in the first seconds, 33 MB, the process 676 MB private and 1.1 GB virtual, 74-79 fps as before; from the disk on the next run. Seen at the screen's own size, the same view on and off: the pavement's cracks and the litter crisper. Not HD: anything with see-through texels (fences, leaves), anything under 64 a side (a car's wheel is a few dozen texels - still soft), draws that aren't indexed. Not checked: the Graphics page with seven rows on it (the row's there by `gfx 1 6 1`); a long story level's memory.
  - *Steady frames only went down* (2026-11-12, a player's report on 0.9.9, RTX 5070 Ti, 2560x1440 at 144 Hz, DXVK, "uncapped": "a memory leak of sorts - from 500 to at most 72 fps", 200+ in 0.9.8; their log: `steady frames - 72 fps (the PC didn't hold 144)`, up after 3 s, down again 0.2 s later, then 72 for good). Not a leak - d3d9.cpp `SteadyRate`, three faults. (1) The rates were the screen's own only (its refresh x4..x1, a half, a third): a scene good for 130 fps was played at 72, one good for 250 at 144 - and the chosen rate wasn't one of them unless it was the screen's: 120 chosen on a 144 Hz screen ran at 72, 60 at 48, 60 on a 75 Hz screen at 37. (2) What a step up needs is measured while paced lower, and that hides the main thread: it makes the next frame while the render thread waits for this one's time, so only the render thread's part shows (`busy`) until the two run back to back - a step up looks free, fails at once. The answer to that was a memory of the failure - not again until frames are a fifth quicker than they were then - but kept for good, and the wait before a try doubled to a minute and never came back: every failed try asked for quicker frames than the last. Over a session the rate only went down. (3) The frames after a pause / a cutscene stall at any rate, and each cost a step (their log: two steps down at every unpause). Now: a ladder (`SteadyLadder`) - the chosen rate, the screen's own, and steps between so none gives up more than about a fifth (144 Hz, uncapped: 432 360 288 240 192 144 120 96 72 60 48; "vsync" keeps the refresh, a half, a third - it waits for the screen's frames); down a step and on while a quarter of the last 60 frames would miss the next too; a failed rate is left alone two minutes (or until frames are a fifth quicker), the wait is three seconds again once a step up held ten; a second's grace when the game comes back (HUD up again); a frame the mod spent reading or handing over an HD texture isn't counted (`VideoFrameExcuse` - HD world textures make hundreds in a level's first minute). Tools: the controller alone against a model of the two threads, 0.9.9's beside it (scratch `steadysim`: the part between `>>> steady controller` marks, built by itself) - the player's case 72 -> 120 in the heavy scene, 144 -> 240 in the light one, one failed try every two minutes; `FGUYMOD_TEST_REFRESH` plays another screen's rate. In the game (FGUY-FPV, 960x540, as a 144 Hz screen): uncapped settles at 144 on this PC (one try at 192 in 30 s), nothing lost at an unpause; 120 chosen runs at 120 (119.8-120.0 over two minutes, two pauses, no change of rate). Not known: what that player's PC is really good for in 0.9.9 against 0.9.8 - if they're at 120 where 0.9.8 gave 200, there's a cost on the main thread still to find (custom characters on four players?). Until a release, `SteadyFrames=0` is 0.9.8's behaviour. **1080p on this PC's Radeon is GPU-bound at 20-30 fps with half a core busy - frame rate tests at 960x540.** The offline Multiverse Madness match ends about 35 s in (the results screen: 60 fps) - the arena without enemies (`start_arena.ps1`) lasts.
  - *The frame rate isn't lowered any more unless asked* (2026-11-12, the user: "Warzone you can go uncapped everything and still have the same frame rate"). Other games' limits never lower the rate for you: a cap, or as many frames as the PC makes, the unevenness left to the screen (G-Sync / FreeSync) and to a clock that doesn't need even frames. So: `[Display] SteadyFrames=1` is the even clock only (steady.cpp); the stepping above is `[Display] SteadyRate=1`, off by default. Without it d3d9.cpp `HoldsChosenRate` only watches whether the chosen rate is held (not once 4 of 60 frames are over; again after 120 in a row that aren't) and gives the clock the exact interval while it is, the frames' own times smoothed (`SteadySetInterval(0)`) while it isn't - "uncapped" (500) is always that. In the game (FGUY-FPV, the arena, 960x540, 240 chosen, a PC good for ~200): 188-223 fps (`240 fps isn't held - the frames as they come`); the same with `SteadyRate=1` sat at 144, and at 48 after a moment's load on the PC (down at once, up a step every few seconds: the very thing reported). Measured, to see what's given up (`FGUYMOD_TRACE_CLOCK=1`, the test pipe's `clocktrace <file>`: every tick's real time and the time the game got, every present; scratch `clockscan.py`: how far the game's time sits off the frames' own, a second's drift taken out): clock only 0.9 ms (68% of frames), the game's own clock 0.3-0.9, the rate held at 144 0.5 (95%: 2.2) - all within a millisecond, and the runs differ more by what else the PC is doing (the user at it: a run fell to 27 fps) than by the setting. Not a clean A/B: for one, a PC nobody's using. **Presents come before the pacing's wait** (Hook_Present: Real_Present, then PaceFrame): at a held rate the frames are shown as they're done (144 held: 5.7..8.2 ms apart), the wait after - evener frames would be the wait first, at the cost of that wait in latency; not done. **keep_screen_on.ps1 by `Start-Process -ArgumentList` needs the path quoted** (this repo's has spaces: it didn't start, the screen saver took two runs' device).
  - *0.9.10, before the release* (2026-11-15). The SETTINGS page's panel holds six rows; GRAPHICS got a seventh with HD world textures and it hung below the panel, over the footer (seen only when the page was looked at: the main menu's Options, `start_test.ps1` with `FGUYMOD_TEST_ARENA=0`, the pipe's `shot` after each pad press) - settingsmenu.as2js `FGUYDrawGfx` now makes the rows lower when a page has more than six (600 / n apart, the texts moved with them). `fpv_holds.dat` was never in a release (make_release.ps1 didn't copy it; 0.9.9's zip has none): players' games learned every hold themselves, a weapon's draw wrong until then. It's in the zip now (dist's: the ten characters' and Peter's five other weapons'); an update overwrites a player's file, their game adds what it learns again. Checked with the zip's own files in FGUY-FPV: Peter's six weapons, zoomed, fists; Lois; three exits from a level in first person back to the menu.
  - *Classic netcode retired*: every match runs free (all five modes already could); the lobby's NETCODE row and the `net` command are gone, the conf field stays for the server.
  - *Updater*: on this PC WinHTTP timed out every time on release-assets.githubusercontent.com (12 s; one of its four addresses doesn't answer) while curl.exe, also Schannel, went on to the next. GetFile: two tries, then System32\curl.exe; the checksum still decides. Tested with GitHub's real 0.9.9 files through FGUYMOD_UPDATE_URL (`updateinstall` on the test pipe).
  - *The tiny gun* (in places on any map: the hand empty, the gun tiny by the crosshair, flickering): whose a draw is goes by the player's feet,
    within 3 (DrawPlayer), and the gun, kept 1.2 ahead of the eye, is 2.2 from them and more: past 3 it was nobody's and stayed at its real place.
    Now an unskinned draw at a hand is that player's, whatever the feet say (`fpv gunbyhand 0` is the old way). Shown with the rig at 2.4 ahead:
    the old way the same picture everywhere, the new way none. Why those places go past 3 at 1.2 isn't known. Ruled out on the way: a gun's
    c0..c3 are the world's (its eye solves to the camera's) - not its own space; forced there the gun's gone. That check held across 81 places;
    a gun is taken as in the world now regardless. Each 5 s at most the log says what by the gun hand was left out.
  - Keyboard mode can't be driven to a match by the test tools (winkey doesn't get past the title; it falls into the demo). `winkey 256` is the left button.
- **The taunt** (2026-11-06): keyboard T = action 48 (keys `0xadc294 + action * 8`); the pad's binding (`0xadbac8 + action * 8`) is 0x20, D-pad
  down - the "gesture" of the power-up notes. Peter puts the gun away and does a routine of the whole body for 7.2 s; firing or moving cuts it
  short and the gun comes back by itself. **`[player+0x22e0]`: 1 exactly while it plays** (0 walking, firing, in a punch; +0x4f68 follows
  the gun, 0 in a punch too; found with `fpv snap` / `fpv diff`). It plays in the idle fidget's layer, so the arms were held: a hand frozen
  in the air, no gun. Now the view goes behind them as for an emote (Taunting, firstperson.cpp). Going and coming back, through their
  head, the inside of the hat filled the screen: theirs not drawn while the pull is 0.05..0.6 (FirstPersonOwnDraw -2; outlines too).
- Still open: the off hand is the game's open hand, placed; no emote pack for Peter to test emoting with; the game's swing trail is drawn
  through the arm; the ink is on every character's arms, tested on Peter only.

- **Wrists** (2026-11-06): each pose also bends each hand at its wrist (`fpv pose`, values 9-14: gun hand, off hand; pitch, yaw, roll in the view). The arm stays where the forearm shows, the hand levels the gun: rolling the whole arm tilted it. Fingers and gun are on the hand, so the grip stays the game's. Arms are zoomed about the middle of the screen, not the gun hand: the game culls each piece from where it really is, and shrunk at the hand's spot the gun's pieces sat at the view's edge.
- **Gun pieces**: `fpv vmcount 1` logs the gun's draws per frame. They go 4 to 2 now and then (looking down, running); frame-by-frame pixel checks (600 frames, both arm modes, before and after the middle-of-screen change) found no visible dip, so what the user saw is not reproduced. T (the taunt): untested - test copies run in pad mode and a keyboard-mode copy didn't get as far as drawing.
- **The off arm** (2026-11-06): spinning the hanging arm round the view to raise it turned the hand wrong way round, the back of it flat at the eye; the wrist can't undo a twisted arm. `fpv offbones 1` logs the hand's bones: in its axes the fingers are +x, the thumb +z, the palm -y. Now the forearm (elbow to hand) and thumb are aimed: two vectors give the whole turn, from each stance's arm (pose values: up, in, twist; 35, 25, 40). Reaching ahead puts the shoulder near the eye, and the nearest bone sets both arms' shrink (the gun jumped, the hand went): the arm is pushed ahead by its own length along the view.
## Controller look: how the right stick turns the camera (2026-11-03)
- **The sticks as floats**: a controller's poll `0x4b5670` (XInputGetState) hands the four shorts LX LY RX RY to
  `0x4b4ec0` (cdecl: out = device state +0x14, the shorts, count 2, center 0, min -32768, max 32767, -7849, 7849 - the
  pushes at `0x4b56cd`); each axis through `0x4bba80`: within +-7849 zero, beyond it stretched, `(v - 7849) / 24918`
  (24919 below zero). A square deadzone, a quarter of the travel, on both sticks. Per stick 7 floats: x, y, the
  direction (2), length, -, angle.
- **The camera's look, `0x581d40`** (a camera method, vtable +0x68 of zFGUYCamThirdPerson - the camera at player
  `+0xf148`; 0x588c40 calls it every frame, then +0x64 = `0x580ab0`): the stick at camera `+0xc0` / `+0xc4`, clipped to
  the unit circle, times `a*m + b*m^2 + c*m^3` (settings `[camera+0x34c]` +0x88 / +0x8c / +0x90: **0, 0, 1 - a cube**);
  then `0x55cc60` per axis: the speed moves toward value * top (+0x7c yaw **3.84 rad/s** / +0x84 pitch 2.09, times
  `0x4fd070` - the aim sensitivity 0xbad810 through 0.75 / 1 / 1.25, lower when zoomed) by accel * dt a frame (+0x78
  **5.76 rad/s^2** / +0x80 3.14, times `0x4fce40`: 0.5 / 1 / 2) - and goes to 0 at once when the curved value is under
  **0.05** (0xadb16c). Out: turn speeds at camera `+0x2c8` (pitch, yaw), which `0x580ab0` adds to the orbit yaw / pitch
  (times camera `+0xcc` and dt). Camera `+0xc8` set: keyboard / mouse (another branch); `+0x300`: held still.
- Measured (the arena, Peter, sensitivity factor 1.25): the cube under 0.05 means **no turn at all below ~52% of the
  stick's travel** (raw 17000); 73% turns 47 deg/s; full push builds up to 275 deg/s over ~0.5 s.
- The **FGUY Controller Look** mod (`src/mods/controllerlook`, a plugin) hooks both: the right stick's shorts remapped so
  the game's own sums give a round deadzone of 8% (left stick untouched), and the look's speeds written over (the game's
  top speed and sensitivity, `t^2`, no build-up, a speed-up held at the edge). Same steps: 30% 17 deg/s, 49% 57, 73%
  148, full 267 at once easing up to ~500 after 0.5 s; zoomed 0.82 of it (the game's own). Its mod.ini `Log=1` logs the
  speeds and how fast the view turned, `Bypass=1` (with Log) the game's own look, to measure it.
- **Why turning looks jagged** (the mod's `Trace=4`: every camera update and every frame drawn, 1280x720 window): one
  camera update a frame, its turn exactly rate * dt (1-2%) - but the game's dt doesn't follow each frame's own time (it
  matches on average; frame to frame +-1.2 ms off, no lag fits better), and frames come unevenly when the PC can't reach
  the cap (240: ~140 fps, frames 4.5-10 ms). The turn seen frame to frame varied 19%; the view sat off a steady turn
  by 1.8 ms of turning (sd; up to 6.7 - at 500 deg/s 0.9 deg, up to 3.4). Capped at a rate the PC holds (120: frames
  8.33 ms, p95 8.4): 6%. The player's place moved on every frame (no 22-a-second steps here, third person, standing
  start in the arena). The user's PC: 3840x2400 borderless on integrated graphics runs a level at ~22-40 fps.
- **Steady frames** (loader: `src/loader/steady.cpp`, d3d9.cpp "steady frames"; `[Display] SteadyFrames=1`, offline):
  the main thread's QPC reads give the tick's own time, last tick's + the interval the pacing holds; the pacing holds
  the highest of the screen's rates (4/3/2/1 x refresh, 1/2, 1/3) the PC can, up to the one chosen. The game's dt then
  is the frames' own (-0.3..+0.4 ms). Measured, 1280x720, FrameRate=240: settles at 120 - the turn frame to frame
  even to 1% (was 19%), the camera eye 1%; 3840x2400: 30 held, 0%. A rate tried up and failed is tried again only once
  frames are a fifth quicker than then (frames paced lower look quicker than they are higher: it went 180 / 120 every
  two seconds, then 60 / 30). The game's dt is QueryPerformanceCounter's (its millisecond clocks not needed).
- **The third-person "lurch"** isn't the run's start: setting off straight is smooth (the eye eases out from 6.0 to
  7.0 behind over ~1 s). Turning while running, the camera met the scenery behind and pulled in from 6.7 to 1.9 in a
  quarter second, in uneven jumps (24 / 34 / 29 / 54 / 70 m/s frame to frame): the camera's collision.
- **The camera's collision, `0x581ff0`** (camera vtable +0x70, from the update 0x588c40 right after the look and the
  turn; thiscall, 13 stack args, ret 0x34: dt first, a state block 10th - camera `+0x22c` - and the orbit block 11th, a
  copy on the stack: pivot [0..2], yaw, pitch, distance [5], eye [6..8]; the 13th a flag it sets). It sweeps a box from
  the pivot to the eye (`0x580320`, cdecl (from, to, frac*): frac, in 1, cut to the first hit; "TtCastCameraDepthDepen-
  Collision"): nearer than the state's distance (+0x14) it goes there at once; farther, it eases out (dt-scaled, capped
  speed). The settings' +0xc is the nearest it comes. Its result goes on through the camera's own easing of the eye
  (camera +0x2a8, then +0x1f0, the eye drawn), which follows it closely: the jumps reach the screen.
- **Eased** (Controller Look, `CameraEase=1`): after the game's sweep, three more where the eye will be 1/3, 2/3 and all
  of 0.25 s ahead (the turn so far, capped at 0.4 rad, and the pivot's move carried on; the hit measured along the line
  from where the pivot will be), and the distance follows the nearest on a critically damped spring (80 ms in, 250 ms
  out), never past the game's. Measured at 60 fps, running and turning past a wall: the pull-in's speed changed by up to
  37 m/s frame to frame, 13-14 eased; spinning in place with no wall about, the camera stays at 6.0.
