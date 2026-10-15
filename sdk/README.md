# Making mods for the FGUY Mod

The FGUY Mod (the base mod) can load separate mods: cosmetics, maps, gameplay changes, new features.
Players switch them on and off on the **MODS** page of the main menu (it takes a restart).

## A mod is a folder

Put it in `<game folder>\FGUYMod\mods\<YourMod>\`:

```
YourMod\
  mod.ini            what it is (required)
  yourmod.dll        a plugin: your own code (optional)
  ui\*.fgpatch       patches for the game's menu / HUD movies (optional)
  files\...          your own versions of game files (optional)
```

### mod.ini

```ini
[Mod]
Name=Better Movement
Version=1.0
Author=you
Description=Slide, slide cancel, tactical sprint.
Type=gameplay
Plugin=movement.dll

[Settings]
SlideSpeed=140
```

- **Type=gameplay**: the mod changes how the game plays. Online, everyone in a lobby must have it
  (the same version, the same files). The lobby server turns away players whose gameplay mods
  differ from the host's and tells them which mods the host has.
- **Type=cosmetic**: it only changes what this PC shows or plays (a skin, a sound, a HUD look).
  Online, the others don't need it. Only use this when it really can't change the game.
  For example, a cosmetic mod that moves a wall would make online matches go out of sync.

### files\

`files\FGUY\Global_Scenes\X.ho` is used instead of the game's `FGUY\Global_Scenes\X.ho`. Any path the game
opens works the same way. If two mods replace the same file, the one later in the alphabet wins.
**Don't share the game's own files.** Share only files you made, or a tool that makes them from
the player's own game.

### ui\

`.fgpatch` files patch the game's Scaleform movies, just as the base mod's do. A patch made for a
movie as the base mod already patched it applies on top of the base mod's patch.

## Plugins (your own code)

A plugin is a **32-bit** Windows DLL (the game is 32-bit) that exports one function:

```cpp
#include "fguymod_plugin.h"

extern "C" __declspec(dllexport) int FGUYModInit(const FGUYModAPI* api) {
    api->Log(L"hello");
    api->OnTick(MyTick, nullptr);
    return 1;
}
```

See `fguymod_plugin.h` for everything the API offers: players, their positions and health, every
player's input for the current tick, a callback every game tick and every frame, and helpers to hook
or patch the game's code. `example\` is a complete small plugin (`build.bat` builds it with Visual
Studio 2022).

### The one rule for online play

Online matches are **lockstep**: every PC runs the same game from the same inputs. A gameplay plugin
may change the game only from things every PC has alike:

- the players' inputs as `api->GetInput` gives them (online, that's the match's synced input)
- the game's own state
- your mod.ini

It may change the game only on the game's thread, in your `OnTick`.

Never use this PC's own keyboard, mouse, clock or random numbers for a gameplay change. They differ
from PC to PC, so the game plays differently on each one and the match goes out of sync. The mod
checks for this all the time: it compares the players' positions, health, money and random numbers,
and if they differ it ends the match and saves a report. Visual changes in `OnFrame` (render thread)
may use anything.

### Testing

Your plugin's `api->Log` lines go to `FGUYMod\FGUYMod_<process id>.log` with your mod's name.
Online, test with two PCs (or the test bot) and watch for `DESYNC` in the logs.
