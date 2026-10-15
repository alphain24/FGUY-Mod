"""Builds dist/FGUYMod/ui/results_<crc>.fgpatch for the end of a match's menu (the results movie in BULK.ho: the
scores, then RETRY / MAIN MENU).

Online the game feeds that menu no mouse (its pointer stayed in the screen's corner), and a click straight on one of
its buttons would happen on this PC only - out of sync. So in an online match its buttons go to the loader instead
(FGUYMOD_MenuMouse: where each one is on the screen, in 1/10000 of it, and which one is chosen now): pointing at
one moves the choice there with Up / Down, a click moves it and presses Enter - keys in the match's input, the same
tick on every PC (src/loader/onlinepause.cpp). The Windows mouse pointer shows meanwhile; the movie's own is
hidden. Offline nothing changes.

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

SOURCE = r'''
function FGUYResultsStart() {
    if (flash.external.ExternalInterface.call('FGUYMOD_OnlineMatch') != true) return;
    Mouse.hide();
    if (_root.FGUYResultsMC != undefined) return;
    var mc = _root.createEmptyMovieClip('FGUYResultsMC', 30500);
    mc.onEnterFrame = FGUYResultsPoll;
}

function FGUYResultsShown(mc) {
    while (mc != undefined && mc != _root) {
        if (!mc._visible) return false;
        mc = mc._parent;
    }
    return true;
}

function FGUYResultsPoll() {
    var vr = Stage.visibleRect;
    var rx = 0, ry = 0, rw = 960, rh = 720;
    if (vr) { rx = vr.x; ry = vr.y; rw = vr.width; rh = vr.height; }
    var refs = buttonReferences, boxes = '';
    if (refs != undefined && refs != null)
        for (var i = 0; i < refs.length; i++) {
            var b = refs[i];
            if (b == undefined || !FGUYResultsShown(b) || b.buttonIndex == undefined) continue;
            var h = b.hitBox != undefined ? b.hitBox : b;
            var bb = h.getBounds(_root);
            if (bb.xMax <= bb.xMin) continue;
            boxes = boxes + Math.round((bb.xMin - rx) / rw * 10000) + ',' + Math.round((bb.yMin - ry) / rh * 10000) + ',' +
                    Math.round((bb.xMax - rx) / rw * 10000) + ',' + Math.round((bb.yMax - ry) / rh * 10000) + ',13,2,' +
                    b.buttonIndex + ',' + CurBtn + ';';
        }
    if (flash.external.ExternalInterface.call('FGUYMOD_MenuMouse', boxes) == '1') Mouse.hide();
}
'''


def build(data):
    p = Patch(data)
    tag, start, end = root_doaction_containing(data, data.index(b'OnRetrySelect'))
    actions = parse_actions(data, start, end)
    o, total = function_at(data, actions, 'OnScreenEnter')
    code_start, code_end, size_field = function2_code(data, o)
    call = A.call('FGUYResultsStart').assemble()
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
