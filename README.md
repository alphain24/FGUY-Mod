# FGUY Mod — Family Guy: Back to the Multiverse (PC)

A community mod project. Goals: online multiplayer first, then cosmetics/skins, new weapons, new maps.
This project never ships game files; players need their own copy of the game.

## Features so far
- **Borderless windowed fullscreen** (sharp on high-DPI / 4K screens), plus forced windowed or
  fullscreen and a custom render resolution.
- **Keep running in the background** when alt-tabbed.
- Override the game's hidden developer settings with your own `fguy.ini`.
- Research tooling: logs of files opened, network traffic, and a virtual controller for test scripts.

## What's here
- `src/loader/` — the mod loader. Builds to `dinput8.dll`, which goes next to `FGUY.exe`.
- `dist/FGUYMod/FGUYMod.ini` — the loader settings file players edit.
- `tools/` — helper scripts (config extraction, test launchers, reverse-engineering helpers).
- `RESEARCH_NOTES.md` — everything we've learned about the game's internals.
- `reference/`, `backups/`, `build/` — local only, never committed (game-derived data / build output).

## Building
Run `build.bat` (needs Visual Studio 2022 with C++). Output: `build\dinput8.dll`.

## Installing
1. Copy `build\dinput8.dll` into the game folder (next to `FGUY.exe`).
2. Copy the `dist\FGUYMod` folder into the game folder and edit `FGUYMod\FGUYMod.ini`.
3. Optional: put a game config at `<game>\FGUYMod\fguy.ini` to override the game's built-in settings.
   Start from a copy of the built-in one (`python tools\extract_ini.py <FGUY.exe> fguy.ini`).
4. Logs go to `<game>\FGUYMod\FGUYMod_<process id>.log`.

To uninstall, delete `dinput8.dll` and the `FGUYMod` folder from the game folder.

## Loader settings (`FGUYMod\FGUYMod.ini`)
| Section / key | Values |
|---|---|
| `[Display] Mode` | `default`, `fullscreen`, `windowed`, `borderless` |
| `[Display] Width`, `Height` | render resolution; `0` = native (borderless) or the game's setting |
| `[Display] RunInBackground` | `1` = keep running when the window isn't active |
| `[Debug] TestInput` | `1` = enable the virtual controller pipe for test scripts |

Environment variables override these for one run (used by test scripts): `FGUYMOD_MODE`,
`FGUYMOD_RES=1280x720`, `FGUYMOD_BACKGROUND=1`, `FGUYMOD_TESTINPUT=1`, `FGUYMOD_INI=server.ini`.

## Test tools
- `tools\start_test.ps1` — start a windowed test copy that runs in the background with the virtual pad.
- `tools\pad.ps1 down down a` — press buttons on the virtual pad.
- `tools\window_shot.ps1 out.png` — capture the game window even when it's behind other windows.
- `tools\test_lan.ps1` — start a server and a client copy side by side.
