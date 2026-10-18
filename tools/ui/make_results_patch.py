"""Builds dist/FGUYMod/ui/results_<crc>.fgpatch for the end of a match's menu (the results movie in BULK.ho: the
scores, then RETRY / MAIN MENU).

Online the game feeds that menu no mouse (its pointer stayed in the screen's corner), and a click straight on one of
its buttons would happen on this PC only - out of sync. So in an online match its buttons go to the loader instead
(FGUYMOD_MenuMouse: where each one is on the screen, in 1/10000 of it, and which one is chosen now): pointing at
one moves the choice there with Up / Down, a click moves it and presses Enter - keys in the match's input, the same
tick on every PC (menumouse.py, src/loader/onlinepause.cpp). Offline nothing changes.

Usage: make_results_patch.py <game folder>
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

HERE = os.path.dirname(os.path.abspath(__file__))
OUT_DIR = os.path.join(HERE, '..', '..', 'dist', 'FGUYMod', 'ui')

from menumouse import SOURCE as MENU_MOUSE  # noqa: E402  (the mouse for the game's menus online)

MENU_DRAW = os.path.join(HERE, '..', '..', 'src', 'ui', 'menudraw.as2js')   # (its text and boxes)

# The rematch vote (src/loader/onlinepause.cpp): Y votes; the bar at the bottom says who has.
REMATCH = r'''
function FGUYRematchDraw() {
    // (online, the match running free: everyone back to the lobby together - the host's pick or everyone's vote
    //  (netplay.cpp NetplayResultsLeave). The HUD that quits the level in the match doesn't run here: this does.)
    if (_root.FGUYResultsQuitting != true && flash.external.ExternalInterface.call('FGUYMOD_ResultsLeave') == true) {
        _root.FGUYResultsQuitting = true;
        getURL('FSCommand:CTRL_DisableButtonInput', '');
        getURL('FSCommand:GAME_Quit', '');
    }
    var s = String(flash.external.ExternalInterface.call('FGUYMOD_Rematch'));
    if (s == _root.FGUYRematchLast) return;
    _root.FGUYRematchLast = s;
    _root.FGUYRematchUI.removeMovieClip();
    if (s == '' || s == 'undefined') return;
    var f = s.split('|');
    var ui = _root.createEmptyMovieClip('FGUYRematchUI', 30550);
    var r = FGUYPauseRect(), size = r.h / 720, names = String(f[3]).split(';'), list = '';
    FGUYPauseN = 0;
    for (var i = 0; i < names.length; i++) {
        if (names[i] == '') continue;
        var p = names[i].split(':');
        list = list + (list == '' ? '' : '     ') + p[0] + (p[1] == '1' ? ': yes' : ': -');
    }
    var text = 'REMATCH  ' + f[0] + ' / ' + f[1] + '      ' + list + '      Y: ' + (f[2] == '1' ? 'take it back' : 'vote');
    FGUYPauseBox(ui, r.x + r.w * 0.2, r.y + r.h * 0.895, r.w * 0.6, r.h * 0.058, 12, 0x13213C, 90, 0x2E5C9A, 100, 2);
    FGUYPauseText(ui, r.x + r.w * 0.2, r.y + r.h * 0.905, r.w * 0.6, r.h * 0.05, text, false, 15 * size, 0xFFFFFF, 'center');
}
'''
SOURCE = MENU_MOUSE + REMATCH + open(MENU_DRAW, encoding='utf-8').read()


def build(data):
    p = Patch(data)
    tag, start, end = root_doaction_containing(data, data.index(b'OnRetrySelect'))
    actions = parse_actions(data, start, end)
    o, total = function_at(data, actions, 'OnScreenEnter')
    code_start, code_end, size_field = function2_code(data, o)
    call = A.call('FGUYMenuMouseStart', 'results').assemble()
    p.replace(code_start, 0, call)
    p.replace(size_field, 2, struct.pack('<H', code_end - code_start + len(call)))
    p.replace(o, 0, as2c.compile_source(SOURCE))  # our functions, defined just before OnScreenEnter
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
        if b'OnRetrySelect' in blob and b'OnScreenEnter' in blob:
            return blob
    sys.exit('results movie not found in BULK.ho')


if __name__ == '__main__':
    data = find_movie(sys.argv[1])
    p = build(data)
    crc = zlib.crc32(data)
    name = f'results_{crc:08x}.fgpatch'
    os.makedirs(OUT_DIR, exist_ok=True)
    open(os.path.join(OUT_DIR, name), 'wb').write(p.to_fgpatch())
    print(f'results movie {len(data)} bytes crc {crc:08x} -> {len(p.apply())} bytes, {len(p.edits)} edits -> {name}')
