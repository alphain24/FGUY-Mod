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

Open **[the Skin Studio](https://alphain24.github.io/FGUY-Mod/)** in Chrome or Edge and click **Open game folder**.
It reads the characters from **your own copy of the game**, right in your browser — nothing is uploaded anywhere.

<details>
<summary>Running it from the project's files instead</summary>

```bash
python tools/cosmetics/dev_server.py "C:/path/to/your/game folder"
```

Then open **http://127.0.0.1:8766**.
</details>

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

**The easy way — in the Skin Studio:** under **Make a mod**, type the mod's name and your name, click **Add packs**
to add any skin (`.fgskin`) or emote (`.fgemote`) packs you've saved, and click **Save as mod (.fgmod)**. The skin
you're working on goes in too. You get `My_Skins.fgmod` — the whole mod in one file.

Put it into the game's **`FGUYMod\mods\`** folder, start the game, and switch it on in *Main Menu › MODS* (mods take
effect after a restart).

<details>
<summary>By hand, from a folder (for mods with more than skins and emotes)</summary>

1. Make a folder named after your mod, with a `mod.ini` and a `skins` folder:

   ```
   MySkins\
   ├── mod.ini
   └── skins\
       ├── Blue_Stewie.fgskin
       └── Goth_Meg.fgskin
   ```

2. Write `mod.ini` (in Notepad):

   ```ini
   [Mod]
   Name=My Skins
   Version=1.0
   Author=Your Name
   Description=A blue Stewie and a goth Meg.
   Type=cosmetic
   ```

3. Pack it into **one file**: in the project's files (the `FGUY-Mod` folder you downloaded), open `tools` and
   **double-click `make_fgmod.py`**. Drag your `MySkins` folder into the window that opens and press Enter. You get
   `MySkins.fgmod` next to the folder.

   > [!NOTE]
   > From a command line instead: open **Command Prompt** (not Python itself — no `>>>` prompt), go to the
   > `FGUY-Mod` folder, and run `py tools\make_fgmod.py "C:\path\to\MySkins"`. Typed from another folder, Windows
   > can't find `tools\make_fgmod.py`.

</details>

That's it: `MySkins.fgmod` is ready to share. Read [Sharing your mod](sharing.md) before you post it.

## Where next?

- [Mod structure](mod-structure.md) — everything a mod can contain, and every `mod.ini` key
- [Outfits & models](models.md) — put a hat on Peter, or replace a character entirely
- [Emotes](emotes.md) — your own animations on the emote wheel
- [Plugins](plugins.md) — your own code
