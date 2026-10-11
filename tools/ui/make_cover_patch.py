"""Builds dist/FGUYMod/ui/cover_<crc>.fgpatch for the split-screen cover movie (cover.gfx, in BULK.ho).

The cover lays itself out for the number of views (SYST_GetNumViewports): with two it draws its two-view
frame, a line between the halves (down the middle side by side, across it top / bottom). Online every PC
shows one full-screen view, so its GetNumViewports first asks the loader (FGUYMOD_CoverViewports: 1 in an
online game, nothing offline - then the game's own answer as before).

Usage: make_cover_patch.py <game folder>
"""
import os
import re
import struct
import sys
import zlib

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from gfxpatch import Patch, function_at, parse_actions, root_doaction_containing  # noqa: E402
import as2c  # noqa: E402

HERE = os.path.dirname(os.path.abspath(__file__))
OUT_DIR = os.path.join(HERE, '..', '..', 'dist', 'FGUYMod', 'ui')

SOURCE = r'''
function GetNumViewports() {
    var mine = flash.external.ExternalInterface.call('FGUYMOD_CoverViewports');
    if (mine != undefined && mine != '') return Number(mine);
    if (!_root.gfxPlayer) return 2;
    return Number(flash.external.ExternalInterface.call('SYST_GetNumViewports'));
}
'''


def build(data):
    p = Patch(data)
    tag, start, end = root_doaction_containing(data, data.index(b'GetNumViewports'))
    actions = parse_actions(data, start, end)
    o, total = function_at(data, actions, 'GetNumViewports')
    p.replace(o, total, as2c.compile_source(SOURCE))
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
        if b'ShowFullscreenCover' in blob and b'Viewports2V' in blob:
            return blob
    sys.exit('cover movie not found in BULK.ho')


if __name__ == '__main__':
    data = find_movie(sys.argv[1])
    p = build(data)
    crc = zlib.crc32(data)
    name = f'cover_{crc:08x}.fgpatch'
    os.makedirs(OUT_DIR, exist_ok=True)
    open(os.path.join(OUT_DIR, name), 'wb').write(p.to_fgpatch())
    print(f'cover {len(data)} bytes crc {crc:08x} -> {len(p.apply())} bytes, {len(p.edits)} edits -> {name}')
