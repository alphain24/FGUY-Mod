"""Builds dist/FGUYMod/ui/loading_<crc>.fgpatch for the loading screens (Levels_Loading\\LD*.ho, and the ones in
Global_Scenes\\FONT.ho).

When the loading is done the game calls the loading screen's PlayExitAnim: it plays its "Out" animation, and at
its end (OnLoadComplete) says FSCommand:SetReadyForTermination - only then does the level start. That animation
runs in real time, so a PC could get there a tick before another: their levels started a tick apart and the
match went out of sync (src/loader/loadsync.cpp). In an online match (FGUYMOD_OnlineMatch) it's skipped: ready at
once, the same tick on every PC. (A match copy's loading screen isn't on screen anyway.) Offline nothing changes.

Usage: make_loading_patch.py <game folder>
"""
import glob
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
function FGUYLoadingExitNow() {
    if (flash.external.ExternalInterface.call('FGUYMOD_OnlineMatch') != true) return;
    if (playingExitAnimation) return;
    playingExitAnimation = true;   // (PlayExitAnim then returns at once)
    OnLoadComplete();
}
'''


def build(data):
    p = Patch(data)
    tag, start, end = root_doaction_containing(data, data.index(b'PlayExitAnim'))
    actions = parse_actions(data, start, end)
    o, total = function_at(data, actions, 'PlayExitAnim')
    code_start, code_end, size_field = function2_code(data, o)
    call = A.call('FGUYLoadingExitNow').assemble()
    p.replace(code_start, 0, call)
    p.replace(size_field, 2, struct.pack('<H', code_end - code_start + len(call)))
    p.replace(o, 0, as2c.compile_source(SOURCE))  # our function, defined just before PlayExitAnim
    p.fix_top_level_jumps(actions)
    assert struct.unpack_from('<H', data, tag)[0] & 0x3f == 0x3f, 'expected long DoAction header'
    p.fix_length_u32(tag + 2, start, end)
    p.fix_length_u32(4, 0, len(data))
    return p


def loading_movies(game_dir):
    seen = {}
    for f in glob.glob(os.path.join(game_dir, 'FGUY', '**', '*.ho'), recursive=True):
        ho = open(f, 'rb').read()
        for m in re.finditer(rb'GFX[\x08-\x0c]', ho):
            o = m.start()
            size = struct.unpack_from('<I', ho, o + 4)[0]
            blob = ho[o:o + size]
            if b'SetReadyForTermination' in blob and b'PlayExitAnim' in blob and b'OnLoadComplete' in blob:
                seen.setdefault(zlib.crc32(blob), (blob, os.path.basename(f)))
    return seen


if __name__ == '__main__':
    os.makedirs(OUT_DIR, exist_ok=True)
    for crc, (data, where) in sorted(loading_movies(sys.argv[1]).items()):
        p = build(data)
        name = f'loading_{crc:08x}.fgpatch'
        open(os.path.join(OUT_DIR, name), 'wb').write(p.to_fgpatch())
        print(f'{where}: loading screen {len(data)} bytes crc {crc:08x} -> {len(p.apply())} bytes, '
              f'{len(p.edits)} edits -> {name}')
