# Testing & troubleshooting

[← Modding guide](README.md)

## The log

Every start of the game writes **`FGUYMod\FGUYMod_<process id>.log`**. It's the first place to look.

| You'll see | It means |
|---|---|
| `Mods: <file>.fgmod unpacked (<n> files)` | Your one-file mod was read (only when it changed). |
| `Mods: the folder X left out - X.fgmod is used` | A folder and a file of the same name: the file wins. |
| `Mods: <name> <version> (cosmetic, on)` | Your mod was found, and whether it's on. |
| `Mods: <name> - plugin started` / `FAILED to start` / `didn't load (error n)` | Your plugin. |
| `Mod <name>: …` | Your plugin's own `Log` lines. |
| `Mods: <path> uses <mod>'s <file>` | A file replacement was used. |
| `UI: "<movie>.gfx" patched with <patch>.fgpatch` | A menu patch was applied. |
| `Cosmetics: player n wears <pack> (<n> textures), with its model` | A skin was put on. |
| `Cosmetics: skin texture <id> made here (<n> recolors, own pixels)` | A skin's texture was built on this PC. |
| `Cosmetics: <file> isn't a skin pack this version can use` | Not a valid skin pack (often: compressed, or no `modelId`). |
| `Models: <name> - <v> vertices, <t> triangles, …` | A model was loaded. |
| `Netplay: player n's skin pack is here (<bytes>)` | Online: someone's skin arrived. |
| `DESYNC` | Online: the PCs' games differed — see [Online play](online.md). |

## Testing offline

1. Put your mod (or loose packs) in place, start the game.
2. *Main Menu › MODS*: check your mod is listed and **ON** (switching takes a restart).
3. Skins: switch the skin **ON** in MODS and play that outfit (Multiverse Madness with one player is a quick way).
4. Emotes: in a level, hold **BACK** / **B**.

## Testing online on one PC

You can run several copies of the game on one PC, each its own player:

1. Start a lobby server on your PC: `py server/fguy_lobby.py`
2. Start two copies that go straight to the ONLINE screen:

   ```powershell
   tools\test_online.ps1 -GameDir "C:\path\to\the game" -Players 2 -Save "C:\path\to\the game\Save\<your save>.sav"
   ```

   Each copy plays from its own copy of the save, with its own name, windowed and muted.
   (`tools\test_online_match.ps1` goes further: hosts, joins, starts a match and reads the logs for you.)
3. Host with one, join with the other, start a match.
4. Close them with `tools\stop_tests.ps1 -GameDir "C:\path\to\the game"` (it closes only the copies it started).

> [!TIP]
> The copies' saves go to `FGUYMod\testsave<n>\`, never over your own.

## Common problems

<details>
<summary><b>My mod isn't on the MODS page</b></summary>

- Is it directly in `FGUYMod\mods\` (not in a subfolder of a subfolder)?
- Does it have a `mod.ini` at its top level (for a `.fgmod`: was it packed from the folder that holds `mod.ini`)?
- Was the `.fgmod` made with `make_fgmod.py`? Zips made by Windows or 7-Zip are compressed and can't be read.
</details>

<details>
<summary><b>My mod is listed but nothing happens</b></summary>

- Is it **ON**? Switching on takes effect at the next start.
- Skins: is the skin itself switched on (offline: its SKIN row in MODS; online: picked in the lobby)?
- Look for your mod's lines in the log.
</details>

<details>
<summary><b>My skin doesn't show</b></summary>

- It's made for one outfit (its `modelId`): play exactly that outfit.
- The log line `Cosmetics: skin texture … - FAILED` means the texture couldn't be built — tell us, with the log.
- Online, it takes a few seconds to reach the others.
</details>

<details>
<summary><b>The body pokes through my outfit</b></summary>

Make sure **Worn over the character** was ticked when you saved the model — only then does the Studio list the
triangles to hide. Pieces must be solid (closed) and thick enough to hide what's inside; see
[Outfits & models](models.md#hiding-the-body-under-the-outfit).
</details>

<details>
<summary><b>My plugin doesn't load</b></summary>

- `didn't load (error 193)`: it's 64-bit — build it as x86.
- `error 126`: it needs a DLL that isn't there — build with `/MT`.
- `has no FGUYModInit`: export it with `extern "C" __declspec(dllexport)`.
</details>

<details>
<summary><b>Online matches go out of sync with my mod</b></summary>

Something your mod changes isn't the same on every PC. Re-read
[the rule for online play](plugins.md#the-rule-for-online-play), and check that your mod is `Type=gameplay`.
</details>

## Reporting a bug in the base mod

Open an issue on the [repository](https://github.com/alphain24/FGUY-Mod/issues) with what happened, roughly when,
and the log (`FGUYMod\FGUYMod_*.log`) — or the report from `FGUYMod\reports\` for online problems.
