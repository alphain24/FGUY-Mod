"""Builds dist/FGUYMod/ui/menu_<crc>.fgpatch for the front-end menu movie (in MNUS.ho).

Changes:
  * Options > Screen > Display Mode gets a third choice, "Borderless".
  * The display-mode getter/setter ask the mod loader (FGUYMOD_GetDisplayMode / FGUYMOD_SetDisplayMode)
    so it can track borderless alongside the game's own windowed flag.
  * The Screen page's OK handler first calls FGUYMOD_ApplyDisplayMode.

Edits are kept minimal (our new code + changed size fields), so the patch carries none of the game's
own script.

Usage: make_menu_patch.py <game folder> [--write-patched <out.gfx>]
"""
import os
import re
import struct
import sys
import zlib

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from gfxpatch import (Patch, Reg, action, define_function, external_call, function_at,  # noqa: E402
                      parse_actions, push, root_doaction_containing)

HERE = os.path.dirname(os.path.abspath(__file__))
OUT_DIR = os.path.join(HERE, '..', '..', 'dist', 'FGUYMod', 'ui')
MOVIE_SIZE = 290635  # the front-end menu movie inside FGUY/Global_Scenes/MNUS.ho

# Original Push record: three constant-pool references, then int 2.
LIST_TAIL = bytes([7, 2, 0, 0, 0])


def find_menu_movie(game_dir):
    ho = open(os.path.join(game_dir, 'FGUY', 'Global_Scenes', 'MNUS.ho'), 'rb').read()
    for m in re.finditer(rb'GFX', ho):
        o = m.start()
        if struct.unpack_from('<I', ho, o + 4)[0] == MOVIE_SIZE and b'GRFX_IsWindowed' in ho[o:o + MOVIE_SIZE]:
            return ho[o:o + MOVIE_SIZE]
    sys.exit('menu movie not found in MNUS.ho')


def function2_code(data, o):
    """(code_start, code_end, codesize_field) of the DefineFunction2 action at `o`."""
    code_start = o + 3 + struct.unpack_from('<H', data, o + 1)[0]
    size = struct.unpack_from('<H', data, code_start - 2)[0]  # header ends with u16 codeSize
    return code_start, code_start + size, code_start - 2


def build(data):
    p = Patch(data)
    anchor = data.index(b'OnDisplayModeValueChanged')
    tag, start, end = root_doaction_containing(data, anchor)
    actions = parse_actions(data, start, end)

    # 1. Display mode choices: [Windowed, Fullscreen] -> [Windowed, Fullscreen, Borderless].
    #    Original: Push 'ButtonListStrings', pool'=MNU_FULLSCREEN', pool'=MNU_WINDOWED', 2 ; InitArray
    #    InitArray pops the top value as element 0, so the new last element is pushed deepest
    #    (right after 'ButtonListStrings').
    for o, code, total in actions:
        body = data[o + 3:o + total]
        if (code == 0x96 and len(body) == 14 and body[0] == 9 and body[3] == 9 and body[6] == 9
                and body[9:] == LIST_TAIL):
            extra = push('Borderless')[3:]                      # the value record only
            p.replace(o + 1, 2, struct.pack('<H', len(body) + len(extra)))
            p.replace(o + 3 + 3, 0, extra)
            p.replace(o + 3 + 10, 4, struct.pack('<i', 3))      # element count 2 -> 3
            break
    else:
        sys.exit('display mode list not found')

    # 2. GetDisplayModeValue() -> return Number(FGUYMOD_GetDisplayMode())
    o, total = function_at(data, actions, 'GetDisplayModeValue')
    body = external_call('FGUYMOD_GetDisplayMode') + action('ToNumber') + action('Return')
    p.replace(o, total, define_function('GetDisplayModeValue', [], body))

    # 3. OnDisplayModeValueChanged(curValue): replace 'needWindowed = (curValue == 0)' with
    #    'needWindowed = FGUYMOD_SetDisplayMode(curValue)' (loader returns the game's windowed flag).
    #    The rest of the function (refreshing the resolution list) is untouched.
    o, total = function_at(data, actions, 'OnDisplayModeValueChanged')
    code_start, code_end, size_field = function2_code(data, o)
    inner = parse_actions(data, code_start, code_end)
    first_setvar = next(a for a, code, t in inner if code == 0x1D)  # SetVariable
    new_code = push('needWindowed') + external_call('FGUYMOD_SetDisplayMode', Reg(2)) + action('SetVariable')
    old_len = first_setvar + 1 - code_start
    p.replace(code_start, old_len, new_code)
    p.replace(size_field, 2, struct.pack('<H', code_end - code_start + len(new_code) - old_len))

    # 4. OnOptionsScreenNextButtonPressed(): first let the loader apply the chosen mode. Switching
    #    between Windowed and Borderless doesn't change the game's own windowed flag, so the original
    #    code would skip GRFX_ChangeResolution.
    o, total = function_at(data, actions, 'OnOptionsScreenNextButtonPressed')
    code_start, code_end, size_field = function2_code(data, o)
    new_code = external_call('FGUYMOD_ApplyDisplayMode') + action('Pop')
    p.replace(code_start, 0, new_code)
    p.replace(size_field, 2, struct.pack('<H', code_end - code_start + len(new_code)))

    # Fix-ups: top-level jumps across edits, DoAction tag length, file length in the GFX header.
    p.fix_top_level_jumps(actions)
    assert struct.unpack_from('<H', data, tag)[0] & 0x3f == 0x3f, 'expected long DoAction header'
    p.fix_length_u32(tag + 2, start, end)
    p.fix_length_u32(4, 0, len(data))
    return p


if __name__ == '__main__':
    data = find_menu_movie(sys.argv[1])
    p = build(data)
    patched = p.apply()
    os.makedirs(OUT_DIR, exist_ok=True)
    name = f'menu_{zlib.crc32(data):08x}.fgpatch'
    open(os.path.join(OUT_DIR, name), 'wb').write(p.to_fgpatch())
    print(f'original {len(data)} bytes crc {zlib.crc32(data):08x} -> patched {len(patched)} bytes, '
          f'{len(p.edits)} edits -> {name}')
    if '--write-patched' in sys.argv:
        open(sys.argv[sys.argv.index('--write-patched') + 1], 'wb').write(patched)
