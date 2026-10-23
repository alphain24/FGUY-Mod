# Getting started

[← Modding guide](README.md)

This page takes you from nothing to a working skin mod that you can share.

## 1. Install the FGUY Mod

1. Download **`FGUYMod-<version>-Setup.exe`** from the
   [releases page](https://github.com/alphain24/FGUY-Mod/releases) and run it.
   (Windows may warn that it's from an unknown publisher: *More info › Run anyway*.)
2. It finds your game and installs the mod into the game's folder — the one with `FGUY.exe`.
3. Start the game. **Main Menu › MODS** lists the mods; **FGUY Cosmetics** and **FGUY Emotes** are already there.

After installing, the game folder looks like this:

```
Family Guy Back to the Multiverse\
├── FGUY.exe                  the game
├── dinput8.dll               the FGUY Mod
└── FGUYMod\
    ├── FGUYMod.ini           settings (open with Notepad)
    ├── mods.ini              which mods are on (the MODS page writes it)
    ├── mods\                 ← mods go here
    │   ├── FGUYCosmetics.fgmod
    │   └── FGUYEmotes.fgmod
    ├── skins\                loose skin packs (yours, straight from the Skin Studio)
    ├── emotes\               loose emote packs
    ├── ui\                   the base mod's menu patches
    └── FGUYMod_<number>.log  a log per game start — your best friend when testing
```

## 2. Get the tools

You only need what you'll use:

| For | You need |
|---|---|
| Skins, outfits, emotes | **Chrome or Edge**, and the Skin Studio (below) |
| Packing a mod into one file, menu patches | **Python 3.8+** |
| Plugins | **Visual Studio 2022** (the *Desktop development with C++* workload) |
| Outfits / models | A 3D program that exports `.glb` — **Blender** is free |

Get the project's files: on the [repository page](https://github.com/alphain24/FGUY-Mod) click
**Code › Download ZIP** (or `git clone` it).

### The Skin Studio

The Skin Studio is a web page that runs on your own PC. It reads the characters from **your own copy of the
game** — nothing is uploaded anywhere. Start it from the project folder:

```bash
python tools/cosmetics/dev_server.py "C:/path/to/your/game folder"
```

Then open **http://127.0.0.1:8766** in Chrome or Edge and click **Open game folder**.

> [!NOTE]
> In browsers without folder access (Firefox, Safari), use **Or pick PLYS.ho** instead and choose
> `FGUY\Global_Scenes\Players\PLYS.ho` from your game folder.

## 3. Make a skin

1. In the Skin Studio, on the **Skins** tab, pick a character and an outfit on the left.
2. Click a colour on the character (or on its texture). Choose a new colour, set **How close** (how similar a
   colour must be to change too), and click **Recolor**. Repeat as often as you like — **Undo** steps back.
3. Want to paint details? **Export PNG**, paint in any art program, **Import PNG**.
4. Give it a name under **Skin pack** and click **Save skin pack**. You get `My_Skin.fgskin`.

Full details: [Skins](skins.md).

## 4. Try it in the game

Copy the `.fgskin` into **`FGUYMod\skins\`** and start the game.

- **Offline:** *Main Menu › MODS* — your skin is listed with a **SKIN** badge. Switch it **ON**: everyone who
  plays that outfit wears it.
- **Online:** on the lobby's character screen, pick the character; your skin has its own tile right after its
  outfit's tile. Everyone in the match sees it.

## 5. Turn it into a mod

A loose skin is just for you. To share it — or a whole collection of skins — make it a mod.

1. Make a folder named after your mod, with a `mod.ini` and a `skins` folder:

   ```
   MySkins\
   ├── mod.ini
   └── skins\
       ├── Blue_Stewie.fgskin
       └── Goth_Meg.fgskin
   ```

2. Write `mod.ini`:

   ```ini
   [Mod]
   Name=My Skins
   Version=1.0
   Author=Your Name
   Description=A blue Stewie and a goth Meg.
   Type=cosmetic
   ```

3. Pack it into **one file**:

   ```bash
   python tools/make_fgmod.py MySkins
   ```

   You get `MySkins.fgmod`, the whole mod in one file.

4. Put `MySkins.fgmod` into `FGUYMod\mods\`, start the game, and switch **My Skins** on in *Main Menu › MODS*
   (mods take effect after a restart).

That's it: `MySkins.fgmod` is ready to share. Read [Sharing your mod](sharing.md) before you post it.

## Where next?

- [Mod structure](mod-structure.md) — everything a mod can contain, and every `mod.ini` key
- [Outfits & models](models.md) — put a hat on Peter, or replace a character entirely
- [Emotes](emotes.md) — your own animations on the emote wheel
- [Plugins](plugins.md) — your own code
