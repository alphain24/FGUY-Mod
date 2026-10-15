"""Builds dist/FGUYMod/ui/pause_<crc>.fgpatch for the game's own pause menu (pause_menu.gfx, in BULK.ho).

Online, the game's pause menu is driven by one player's controller on every PC alike (src/loader/onlinepause.cpp:
in story co-op it comes up when anyone pauses, as offline; in the multiplayer modes once every player has the online
pause menu open). Its home page online:
  * story co-op: everything the offline story has - RESUME, OPTIONS, RESTART, MISSION SELECT, QUIT GAME - but LEAVE
    CO-OP (online you leave from the lobby). Restart and the mission select happen on every PC together.
  * the multiplayer modes: RESUME and QUIT GAME (Restart there would restart the match for everyone).
Its OPTIONS would change the settings of everyone's game (the menu runs on every PC), so online it opens the player
who paused their own SETTINGS page (the online one: each player's settings, FGUYMOD_PauseSettings) instead.
The mouse works in it (menumouse.py). Offline nothing changes.

Usage: make_pause_patch.py <game folder>
"""
import os
import re
import struct
import sys
import zlib

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from gfxpatch import Patch, function_at, parse_actions, root_doaction_containing  # noqa: E402
import as2asm as A  # noqa: E402
import as2c  # noqa: E402
from make_menu_patch import function2_code  # noqa: E402
from menumouse import SOURCE as MENU_MOUSE  # noqa: E402

HERE = os.path.dirname(os.path.abspath(__file__))
OUT_DIR = os.path.join(HERE, '..', '..', 'dist', 'FGUYMod', 'ui')
MENU_DRAW = os.path.join(HERE, '..', '..', 'src', 'ui', 'menudraw.as2js')   # the online pause menu's drawing

SOURCE = r'''
function FGUYPauseOnlineButtons() {
    if (flash.external.ExternalInterface.call('FGUYMOD_OnlineMatch') != true) return;
    FGUYMenuMouseStart('pause');
    FGUYPauseOverlayStart();
    var story = flash.external.ExternalInterface.call('FGUYMOD_StoryCoop') == true;
    var keep = [];
    for (var i = 0; i < pauseScreenButtonInfo.length; i++) {
        var n = pauseScreenButtonInfo[i].name;
        if (n == 'Resume' || n == 'SaveQuit' || (story && (n == 'Options' || n == 'Restart' || n == 'LevelSelect')))
            keep.push(pauseScreenButtonInfo[i]);
    }
    if (keep.length == 0) return;
    pauseScreenButtonInfo.splice(0, pauseScreenButtonInfo.length);
    for (var k = 0; k < keep.length; k++) pauseScreenButtonInfo.push(keep[k]);
}

// The player's SETTINGS page over this menu while it's up (drawn as the online pause menu draws it: menudraw.as2js).
function FGUYPauseOverlayStart() {
    if (_root.FGUYOverlayMC != undefined) return;
    var mc = _root.createEmptyMovieClip('FGUYOverlayMC', 30600);
    mc.onEnterFrame = FGUYPauseOverlayPoll;
}

function FGUYPauseOverlayPoll() {
    var s = String(flash.external.ExternalInterface.call('FGUYMOD_PauseMenu'));
    var f = s.split('|');
    flash.external.ExternalInterface.call('FGUYMOD_PauseCursor');   // (keeps the Windows pointer up meanwhile)
    if (f[0] == '1' && f[6] == '1') {
        if (s != _root.FGUYOverlayLast) {
            _root.FGUYOverlayLast = s;
            FGUYPauseDraw(f);
            // (drawn in screen units: this movie's root is scaled and moved on the screen - undone for the page)
            var ui = _root.FGUYPauseUI, zx = _root._xscale / 100, zy = _root._yscale / 100;
            if (ui != undefined && zx > 0 && zy > 0) {
                ui._xscale = 100 / zx;
                ui._yscale = 100 / zy;
                ui._x = -_root._x / zx;
                ui._y = -_root._y / zy;
            }
        }
    } else if (_root.FGUYOverlayLast != undefined && _root.FGUYOverlayLast != '') {
        _root.FGUYOverlayLast = '';
        _root.FGUYPauseUI.removeMovieClip();
    }
}

// OPTIONS online: the player who paused gets their own settings (the loader's page over the game's menu).
function FGUYPauseOptions() {
    if (flash.external.ExternalInterface.call('FGUYMOD_OnlineMatch') != true) return false;
    flash.external.ExternalInterface.call('FGUYMOD_PauseSettings');
    return true;
}
''' + MENU_MOUSE + open(MENU_DRAW, encoding='utf-8').read()


def build(data):
    p = Patch(data)
    tag, start, end = root_doaction_containing(data, data.index(b'OnHomeEnter'))
    actions = parse_actions(data, start, end)
    o, total = function_at(data, actions, 'OnHomeEnter')
    code_start, code_end, size_field = function2_code(data, o)
    call = A.call('FGUYPauseOnlineButtons').assemble()
    p.replace(code_start, 0, call)
    p.replace(size_field, 2, struct.pack('<H', code_end - code_start + len(call)))
    p.replace(o, 0, as2c.compile_source(SOURCE))  # our functions, defined just before OnHomeEnter
    # OPTIONS: if (FGUYPauseOptions()) return;  (OnOptionsSelect is an old-style function: its header ends the same way)
    o2, _ = function_at(data, actions, 'OnOptionsSelect')
    c2_start, c2_end, size2 = function2_code(data, o2)
    guard = (A.Code(A.callv('FGUYPauseOptions'), A.op('Not')).if_true('game') +
             A.Code(A.pushes(False), A.op('Return')).label('game')).assemble()
    p.replace(c2_start, 0, guard)
    p.replace(size2, 2, struct.pack('<H', c2_end - c2_start + len(guard)))
    p.fix_top_level_jumps(actions)
    assert struct.unpack_from('<H', data, tag)[0] & 0x3f == 0x3f, 'expected long DoAction header'
    p.fix_length_u32(tag + 2, start, end)
    p.fix_length_u32(4, 0, len(data))
    return p


def find_movie(game_dir):
    ho = open(os.path.join(game_dir, 'FGUY', 'Global_Scenes', 'BULK.ho'), 'rb').read()
    for m in re.finditer(rb'GFX[\x08-\x0c]', ho):
        o = m.start()
        size = struct.unpack_from('<I', ho, o + 4)[0]
        blob = ho[o:o + size]
        if b'pauseScreenButtonInfo' in blob and b'OnHomeEnter' in blob:
            return blob
    sys.exit('pause menu movie not found in BULK.ho')


if __name__ == '__main__':
    data = find_movie(sys.argv[1])
    p = build(data)
    crc = zlib.crc32(data)
    name = f'pause_{crc:08x}.fgpatch'
    os.makedirs(OUT_DIR, exist_ok=True)
    open(os.path.join(OUT_DIR, name), 'wb').write(p.to_fgpatch())
    print(f'pause menu {len(data)} bytes crc {crc:08x} -> {len(p.apply())} bytes, {len(p.edits)} edits -> {name}')
