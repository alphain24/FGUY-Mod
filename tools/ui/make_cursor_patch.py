"""Builds dist/FGUYMod/ui/cursor_<crc>.fgpatch for the game's mouse pointer (a small movie in BULK.ho: useCursor,
moved by the mouse's onMouseMove).

Online the game feeds its menus no mouse during a match, so this pointer sat in the screen's top left corner (the end
of the match's menu showed it there). Online the menus' pointer is the Windows one (src/loader/display.cpp
DisplayOsPointer), so in an online match this one stays hidden. Offline nothing changes.

Usage: make_cursor_patch.py <game folder>
"""
import os
import re
import struct
import sys
import zlib

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from gfxpatch import Patch, parse_actions, root_doaction_containing  # noqa: E402
import as2asm as A  # noqa: E402
import as2c  # noqa: E402

HERE = os.path.dirname(os.path.abspath(__file__))
OUT_DIR = os.path.join(HERE, '..', '..', 'dist', 'FGUYMod', 'ui')

SOURCE = r'''
function FGUYCursorStart() {
    FGUYCursorWait = 0;
    FGUYCursorOnline = false;
    FGUYCursorHid = false;
    var mc = _root.createEmptyMovieClip('FGUYCursorMC', 999);
    mc.onEnterFrame = FGUYCursorPoll;
}

function FGUYCursorPoll() {
    FGUYCursorWait = FGUYCursorWait - 1;
    if (FGUYCursorWait <= 0) {   // (asked once a second)
        FGUYCursorWait = 30;
        FGUYCursorOnline = flash.external.ExternalInterface.call('FGUYMOD_OnlineMatch') == true;
    }
    if (FGUYCursorOnline) {
        useCursor._visible = false;
        FGUYCursorHid = true;
    } else if (FGUYCursorHid) {   // (the match is over: as the game had it)
        useCursor._visible = true;
        FGUYCursorHid = false;
    }
}
'''


def build(data):
    p = Patch(data)
    tag, start, end = root_doaction_containing(data, data.index(b'UpdateCursorLR'))
    actions = parse_actions(data, start, end)
    first = actions[1][0] if actions[0][1] == 0x88 else actions[0][0]   # (after the constant pool)
    p.replace(first, 0, as2c.compile_source(SOURCE))   # our functions, first
    end_at = [o for o, code, _ in actions if code == 0][-1]   # the root code's End: our start just before it
    p.replace(end_at, 0, A.call('FGUYCursorStart').assemble())
    p.fix_top_level_jumps(actions)
    if struct.unpack_from('<H', data, tag)[0] & 0x3f == 0x3f:
        p.fix_length_u32(tag + 2, start, end)
    else:
        sys.exit('expected a long DoAction header')
    p.fix_length_u32(4, 0, len(data))
    return p


def find_movie(game_dir):
    ho = open(os.path.join(game_dir, 'FGUY', 'Global_Scenes', 'BULK.ho'), 'rb').read()
    for m in re.finditer(rb'GFX[\x08-\x0c]', ho):
        o = m.start()
        size = struct.unpack_from('<I', ho, o + 4)[0]
        blob = ho[o:o + size]
        if b'UpdateCursorLR' in blob and b'useCursor' in blob:
            return blob
    sys.exit('pointer movie not found in BULK.ho')


if __name__ == '__main__':
    data = find_movie(sys.argv[1])
    p = build(data)
    crc = zlib.crc32(data)
    name = f'cursor_{crc:08x}.fgpatch'
    os.makedirs(OUT_DIR, exist_ok=True)
    open(os.path.join(OUT_DIR, name), 'wb').write(p.to_fgpatch())
    print(f'pointer movie {len(data)} bytes crc {crc:08x} -> {len(p.apply())} bytes, {len(p.edits)} edits -> {name}')
