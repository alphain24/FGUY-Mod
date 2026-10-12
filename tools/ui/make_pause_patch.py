"""Builds dist/FGUYMod/ui/pause_<crc>.fgpatch for the game's own pause menu (pause_menu.gfx, in BULK.ho).

Online, the game's pause menu only comes up once every player has the online pause menu open (the loader
pauses the game then: src/loader/onlinepause.cpp), and it's driven by one player's controller on every PC. Its
Options would change that player's settings - sound included - on everybody's PC, and Restart would restart the
level for everyone. So in an online match its home page keeps only RESUME and QUIT GAME (FGUYMOD_OnlineMatch);
each player's settings are in the online menu's SETTINGS. Offline nothing changes.

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

HERE = os.path.dirname(os.path.abspath(__file__))
OUT_DIR = os.path.join(HERE, '..', '..', 'dist', 'FGUYMod', 'ui')

SOURCE = r'''
function FGUYPauseOnlineButtons() {
    if (flash.external.ExternalInterface.call('FGUYMOD_OnlineMatch') != true) return;
    var keep = [];
    for (var i = 0; i < pauseScreenButtonInfo.length; i++) {
        var n = pauseScreenButtonInfo[i].name;
        if (n == 'Resume' || n == 'SaveQuit') keep.push(pauseScreenButtonInfo[i]);
    }
    if (keep.length == 0) return;
    pauseScreenButtonInfo.splice(0, pauseScreenButtonInfo.length);
    for (var k = 0; k < keep.length; k++) pauseScreenButtonInfo.push(keep[k]);
}
'''


def build(data):
    p = Patch(data)
    tag, start, end = root_doaction_containing(data, data.index(b'OnHomeEnter'))
    actions = parse_actions(data, start, end)
    o, total = function_at(data, actions, 'OnHomeEnter')
    code_start, code_end, size_field = function2_code(data, o)
    call = A.call('FGUYPauseOnlineButtons').assemble()
    p.replace(code_start, 0, call)
    p.replace(size_field, 2, struct.pack('<H', code_end - code_start + len(call)))
    p.replace(o, 0, as2c.compile_source(SOURCE))  # our function, defined just before OnHomeEnter
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
