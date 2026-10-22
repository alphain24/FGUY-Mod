# FGUY Mod: Family Guy: Back to the Multiverse (PC)

A free community mod that adds **online multiplayer** to Family Guy: Back to the Multiverse, plus fixes
and quality-of-life for the PC version. You need your own copy of the game; this mod contains none of its files.

## Install

1. Download **FGUYMod-<version>-Setup.exe** from the [latest release](https://github.com/alphain24/FGUY-Mod/releases/latest).
2. Run it. It finds the game (or asks where it is) and installs the mod. Windows may warn that the file is
   from an unknown publisher (it isn't code-signed): click **More info > Run anyway**.
3. Start the game as usual.

Run the setup again any time to update or remove the mod. The mod also tells you on the ONLINE screen when
there's a new version, and installs it for you.

## What it adds

**Online play** (Main Menu > ONLINE)
- Host or join lobbies (public, or by code), pick character, skin and team; the host picks mode, map and rules.
- Multiplayer modes and Story co-op, every player with their own full screen.
- Keyboard and mouse or controller, with your own settings.
- Lobby chat, pings in the match, rematch vote, spectating in Multiverse Madness.
- **Rejoin**: left a match or your game closed? Its lobby shows REJOIN MATCH while it's still on.
- Story co-op: one player's pause pauses everyone, with Restart Level and Mission Select.
- Matches stay in sync (lockstep, checked all the time); if one ever doesn't, a report is saved in
  `FGUYMod\reports` to send in.

**Mods** (Main Menu > MODS)
- Put separate mods in `FGUYMod\mods\` and switch them on and off. Making one: see [`sdk/`](sdk/README.md).

**FGUY Cosmetics and FGUY Emotes** (two mods that come with it, on the MODS page; they grow with each update)
- **Cosmetics**: new skins (and later models) for the characters. Online, pick a skin on the lobby's character
  screen - everyone sees it; offline, switch one on in MODS.
- **Emotes**: in a level, **hold BACK** (controller) or **B** (keyboard) for the emote wheel, point at an emote with
  the left stick or the mouse (LB / RB or the mouse wheel: more), let go to play it. Only the joints an emote moves
  leave the game's own animation, so a wave works while walking. Online, everyone sees it.

**PC fixes**
- Runs on **Vulkan** ([DXVK](https://github.com/doitsujin/dxvk), the translation Valve's Proton uses) for
  smoother, more reliable play on modern PCs. A PC without Vulkan 1.3 falls back to DirectX 9 by itself; to always
  use DirectX 9, set `Renderer=d3d9` in `FGUYMod\FGUYMod.ini`.
- Borderless fullscreen, every resolution your monitor has (4K too), frame rate cap or uncapped.
- The Windows key works; keeps running when you alt-tab (optional).

## Settings

`FGUYMod\FGUYMod.ini` in the game's folder (open with Notepad): display mode, frame rate, online name,
lobby server. Most display settings are also in the game's own Options > Screen.

## For developers

- `src/loader/`: the mod itself, a `dinput8.dll` loaded by the game. Build with `build.bat`
  (Visual Studio 2022, C++). Online play: `netplay.cpp` (lockstep), `online.cpp` (lobby), `handoff.cpp`.
- `src/ui/`, `tools/ui/`: the in-game screens (Scaleform ActionScript, compiled into `dist/FGUYMod/ui/*.fgpatch`
  patches that hold only our changes, never game data).
- `server/`: the lobby + relay server (one Python file, see its README).
- `tools/cosmetics/`: the Skin Studio (makes skin and emote packs; a static web page, `dev_server.py` serves it
  locally). `dist/FGUYMod/mods/`: the FGUY Cosmetics and FGUY Emotes mods. In the game:
  `cosmetics.cpp` (skins), `motion.cpp` (characters' poses), `emotes.cpp`.
- `tools/re/`: research tools - read `.ho` packages, export characters (rigged, with recorded motions) as `.glb`.
- `src/installer/`, `tools/make_release.ps1`: the setup and the release files.
- `tools/test_*.ps1`: automated tests that run copies of the game on one PC.
- `RESEARCH_NOTES.md`: what we've learned about the game's internals.

DXVK (`dist/FGUYMod/dxvk`) is by Philip Rebohle and contributors, under the zlib licence.

The online loading screen's music (a re-recording, in the release downloads only - not in this repository) is by
Andrew Stockton.

This mod was made using AI (Claude, by Anthropic).
