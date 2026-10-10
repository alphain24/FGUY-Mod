"""Builds dist/FGUYMod/ui/start_<crc>.fgpatch for the title screen movie (start_screen.gfx, in MNUS.ho).

In an online session (src/ui/session.as2js) every player's game must get from boot to the menus on its
own and in step: when the title screen has animated in (AnimateInDone), it asks the loader to press
Start and then A (the PC's autosave notice) on the host's controller - on every PC alike.

Usage: make_start_patch.py <game folder>
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
function FGUYTitleSession() {
    if (FGUYTitleDone) return;
    var cfg = flash.external.ExternalInterface.call('FGUYMOD_SessionConfig');
    var auto = flash.external.ExternalInterface.call('FGUYMOD_TestAutoMenu');
    if ((cfg == undefined || cfg == '') && (auto == undefined || auto == '')) return;
    FGUYTitleDone = true;
    flash.external.ExternalInterface.call('FGUYMOD_SessionLog', 'title screen');
    flash.external.ExternalInterface.call('FGUYMOD_SessionPress', 0, 'start', 60);
    flash.external.ExternalInterface.call('FGUYMOD_SessionPress', 0, 'a', 160);
}
'''


def build(data):
    p = Patch(data)
    tag, start, end = root_doaction_containing(data, data.index(b'AnimateInDone'))
    actions = parse_actions(data, start, end)
    o, total = function_at(data, actions, 'AnimateInDone')
    code_start, code_end, size_field = function2_code(data, o)
    call = A.call('FGUYTitleSession').assemble()
    p.replace(code_start, 0, call)
    p.replace(size_field, 2, struct.pack('<H', code_end - code_start + len(call)))
    p.replace(o, 0, as2c.compile_source(SOURCE))  # our function, defined just before AnimateInDone
    p.fix_top_level_jumps(actions)
    assert struct.unpack_from('<H', data, tag)[0] & 0x3f == 0x3f, 'expected long DoAction header'
    p.fix_length_u32(tag + 2, start, end)
    p.fix_length_u32(4, 0, len(data))
    return p


def find_movie(game_dir):
    ho = open(os.path.join(game_dir, 'FGUY', 'Global_Scenes', 'MNUS.ho'), 'rb').read()
    for m in re.finditer(rb'GFX[\x08-\x0c]', ho):
        o = m.start()
        size = struct.unpack_from('<I', ho, o + 4)[0]
        blob = ho[o:o + size]
        if b'AnimateInDone' in blob and b'GotoMainMenu' in blob and b'StartPressed' in blob:
            return blob
    sys.exit('title screen movie not found in MNUS.ho')


if __name__ == '__main__':
    data = find_movie(sys.argv[1])
    p = build(data)
    crc = zlib.crc32(data)
    name = f'start_{crc:08x}.fgpatch'
    os.makedirs(OUT_DIR, exist_ok=True)
    open(os.path.join(OUT_DIR, name), 'wb').write(p.to_fgpatch())
    print(f'title screen {len(data)} bytes crc {crc:08x} -> {len(p.apply())} bytes, {len(p.edits)} edits -> {name}')
