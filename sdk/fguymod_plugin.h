/* FGUY Mod plugin SDK - write a mod for Family Guy: Back to the Multiverse (PC) as a native DLL.
 *
 * A mod is a folder in <game>\FGUYMod\mods\<YourMod>\ with a mod.ini:
 *
 *     [Mod]
 *     Name=Better Movement
 *     Version=1.0
 *     Author=you
 *     Description=Slide, tactical sprint, ...
 *     Type=gameplay            ; gameplay: everyone in an online lobby must have it, the same version
 *                              ; cosmetic: only changes what this PC shows or hears (may differ online)
 *     Plugin=movement.dll      ; optional: this DLL is loaded when the mod is on
 *
 *     [Settings]               ; optional: your own settings (api->Setting reads them)
 *     SlideSpeed=140
 *
 * and optionally ui\*.fgpatch (menu / HUD movie patches, like the base mod's) and files\... (game files, by their
 * path in the game folder - files\FGUY\Global_Scenes\X.ho is used instead of FGUY\Global_Scenes\X.ho).
 * Players switch mods on and off on the MODS page of the main menu (it takes a restart).
 *
 * The plugin: a 32-bit DLL (the game is 32-bit) that exports
 *
 *     extern "C" __declspec(dllexport) int FGUYModInit(const FGUYModAPI* api);
 *
 * called once, early at game start (as the game creates its window - before its menus and levels). Return 1 if all
 * went well.
 * Keep the api pointer: it stays valid for the game's lifetime.
 *
 * ONLINE PLAY IS LOCKSTEP: every PC runs the same game from the same inputs. A gameplay mod must therefore change the
 * game only from things every PC has alike - the players' inputs as api->GetInput gives them (the match's synced
 * input), the game's own state, your mod.ini - and only on the game's thread (OnTick). Never from this PC's own
 * keyboard / mouse / clock / random numbers: that plays differently on each PC and the match goes out of sync.
 * Purely visual or audio changes (OnFrame, the render thread) are free to use anything.
 */
#ifndef FGUYMOD_PLUGIN_H
#define FGUYMOD_PLUGIN_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define FGUYMOD_API_VERSION 1

/* One player's input for the current tick (online: the match's synced input - the same on every PC). */
typedef struct FGUYModInput {
    uint16_t buttons;            /* XInput buttons (XINPUT_GAMEPAD_A ...) */
    uint8_t leftTrigger, rightTrigger;
    int16_t leftX, leftY, rightX, rightY;
    int keyboardPlayer;          /* 1: this player plays with keyboard and mouse */
    uint8_t keys[32];            /* keyboard: bit (vk & 7) of keys[vk >> 3] is set while key vk is down */
    int16_t mouseX, mouseY;      /* mouse movement this tick */
    int16_t mouseWheel;
    uint8_t mouseButtons;        /* 1 left, 2 right, 4 middle */
} FGUYModInput;

typedef void (*FGUYModCallback)(void* user);

typedef struct FGUYModAPI {
    int version;                              /* FGUYMOD_API_VERSION (newer loaders only add fields at the end) */
    const wchar_t* modFolder;                 /* this mod's folder (no trailing backslash) */

    void (*Log)(const wchar_t* text);         /* a line in the mod's log (FGUYMod\FGUYMod_<pid>.log), with the mod's name */
    int (*Setting)(const wchar_t* key, int defaultValue);   /* mod.ini [Settings] key, as a number */

    int (*IsOnline)(void);                    /* 1 in an online match (lockstep: see above) */
    int (*LocalPlayer)(void);                 /* online: this PC's player index; offline: 0 */
    unsigned (*Tick)(void);                   /* the match's tick number (online; offline: frames counted) */
    int (*TickRate)(void);                    /* ticks per second (online: the match's rate; offline: measured - the
                                                 game then ticks once per frame drawn) */

    int (*GetPlayers)(void** out, int max);   /* the level's players, in player-index order; returns how many */
    int (*PlayerIndex)(void* player);         /* 0..3 */
    float* (*PlayerPosition)(void* player);   /* x, y, z (writable) */
    float (*PlayerHealth)(void* player);      /* 0: down; 200: full */
    int (*GetInput)(int playerIndex, FGUYModInput* out);   /* 1 if known: online every player's, offline this PC's */

    int (*OnTick)(FGUYModCallback fn, void* user);    /* every game tick, on the game's thread, before the game runs
                                                         it (the place for gameplay changes) */
    int (*OnFrame)(FGUYModCallback fn, void* user);   /* every frame the render thread draws (visual only) */

    /* Detour: the first `length` bytes at `target` (whole instructions, at least 5, no jumps or calls among them) are
       moved to a trampoline, and target jumps to `detour`. *original gets the trampoline: call it to run the game's
       function. Returns 1 on success. */
    int (*Hook)(void* target, int length, void* detour, void** original);
    /* Write bytes into the game's code (with the page made writable). Returns 1 on success. */
    int (*Patch)(void* address, const void* bytes, int length);
} FGUYModAPI;

typedef int (*FGUYModInitFn)(const FGUYModAPI* api);

#ifdef __cplusplus
}
#endif

#endif
