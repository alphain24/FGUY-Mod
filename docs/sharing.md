# Sharing your mod

[← Modding guide](README.md)

## Before you share

- [ ] Packed into one file: `python tools/make_fgmod.py YourMod`
- [ ] `mod.ini` has a clear **Name**, **Version**, **Author** and **Description**
- [ ] The right **Type** — `gameplay` if it changes how the game plays in any way
- [ ] Tested offline, and online if it's a gameplay mod (see [Testing](testing.md))
- [ ] **Nothing of the game's own in it** (below)

## Only your own work

> [!CAUTION]
> **Never share the game's own files or art** — its `.ho` packages, textures, models, sounds or movies, changed or
> not. They belong to the game's makers, and every player already has them.

Everything here is built so you never need to:

| You share | Not | Because |
|---|---|---|
| A skin pack: recolours + the pixels **you** painted | The recoloured texture | Each player's PC rebuilds the texture from their own game. |
| A `.fgpatch`: your bytes + where they go | A changed movie | The patch is applied to the player's own movie. |
| Your own model, made from scratch | The game's models | |
| A tool that changes a game file on the player's PC | The changed game file | |

Pictures and models you make *of* the characters (a drawing of Peter in a hat, an outfit for him) are fan work —
make them yourself rather than cutting them out of the game.

## How players install it

Tell them:

1. Install the **FGUY Mod** ([releases](https://github.com/alphain24/FGUY-Mod/releases)).
2. Put **`YourMod.fgmod`** into the game's **`FGUYMod\mods\`** folder (on *Main Menu › MODS*, **Y** opens it).
3. Restart the game and switch **Your Mod** on in *Main Menu › MODS*.

To update, they replace the file with the new one (keep the file name the same, or the mod is seen as a new one and
starts off).

## Versioning

- Raise `Version` whenever you release a change.
- For gameplay mods, everyone in an online lobby needs the same version: the lobby tells players which version the
  host has.
- Keep the **file name** stable: whether a mod is on is remembered by it.

## Getting it into the FGUY Cosmetics or Emotes mod

Made a skin, an outfit or an emote you'd like everyone to have? The two official mods grow with each update. Open
an issue or a pull request on the [repository](https://github.com/alphain24/FGUY-Mod) with your pack and a
screenshot. Packs live in [`dist/FGUYMod/mods/`](../dist/FGUYMod/mods/) and are packed into
`FGUYCosmetics.fgmod` / `FGUYEmotes.fgmod` for each release.
