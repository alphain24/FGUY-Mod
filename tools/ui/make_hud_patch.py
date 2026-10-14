"""Builds dist/FGUYMod/ui/hud_<crc>.fgpatch for every in-game HUD movie (hud.gfx: the Multiplayer one in
PLYS.ho, the Story one in PLYW.ho / PLYX.ho).

Online, each PC shows only its own player full screen (src/loader/splitview.cpp), so the HUD must lay
out as single player too. The game drives the HUD's layout by calling two of its functions:
  * SetSplitScreenModeInternal(ScreenFrame, ScreenMode): which layout (0 = one viewport).
  * Viewport numbers, which the HUD turns into its objects' names in GetObjectPath, IsViewportValid
    and UpdateViewportFrameMapping.
We insert at the start of each: ask the loader (FGUYMOD_HudSplitMode / FGUYMOD_HudViewport: my
viewport -> 0, the others -> -1, i.e. no HUD) and use its answer. Without online play the loader hands back the game's own values unchanged.

Usage: make_hud_patch.py <game folder>
"""
import glob
import os
import re
import struct
import sys
import zlib

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from gfxpatch import Patch, Reg, function_at, parse_actions, root_doaction_containing  # noqa: E402
import as2asm as A  # noqa: E402
import as2c  # noqa: E402
from make_menu_patch import function2_code  # noqa: E402
from hudscale import layout_table  # noqa: E402

HERE = os.path.dirname(os.path.abspath(__file__))
OUT_DIR = os.path.join(HERE, '..', '..', 'dist', 'FGUYMod', 'ui')
PAUSE_SOURCE = os.path.join(HERE, '..', '..', 'src', 'ui', 'pause.as2js')   # the online pause menu


def param_registers(data, o):
    """{param name: register} of the DefineFunction2 at `o`."""
    p = o + 3
    p = data.index(b'\0', p) + 1                      # name
    nparams, _, _ = struct.unpack_from('<HBH', data, p)
    p += 5
    regs = {}
    for _ in range(nparams):
        reg = data[p]
        end = data.index(b'\0', p + 1)
        regs[data[p + 1:end].decode()] = reg
        p = end + 1
    return regs


def insert_at_start(p, data, actions, name, code):
    o, _ = function_at(data, actions, name)
    code_start, code_end, size_field = function2_code(data, o)
    body = code(param_registers(data, o)).assemble()
    p.replace(code_start, 0, body)
    p.replace(size_field, 2, struct.pack('<H', code_end - code_start + len(body)))


def build(data):
    p = Patch(data)
    anchor = data.index(b'SetSplitScreenModeInternal')
    tag, start, end = root_doaction_containing(data, anchor)
    actions = parse_actions(data, start, end)

    def split_mode(r):  # ScreenFrame = ScreenMode = FGUYMOD_HudSplitMode(ScreenMode)
        frame, mode = r['ScreenFrame'], r['ScreenMode']
        return (A.store(mode, A.Code(A.external('FGUYMOD_HudSplitMode', Reg(mode)), A.op('ToNumber'))) +
                A.store(frame, Reg(mode)))

    def viewport(r):    # ViewportIndex = FGUYMOD_HudViewport(ViewportIndex)
        vp = r['ViewportIndex']
        return A.store(vp, A.Code(A.external('FGUYMOD_HudViewport', Reg(vp)), A.op('ToNumber')))

    # (the online pause menu starts with the HUD's first layout; offline its poll finds nothing to do; and the
    #  damage frame is stretched to the screen's width - offline too: it's a plain bug fix)
    insert_at_start(p, data, actions, 'SetSplitScreenModeInternal',
                    lambda r: split_mode(r) + A.Code(A.call('FGUYPauseStart')) + A.Code(A.call('FGUYFitDamageFx')))
    o, _ = function_at(data, actions, 'SetSplitScreenModeInternal')
    # (where and how big each element is in the single-player HUD and in each 4-way quarter, from this movie:
    #  see FGUYHudZoom)
    table = ', '.join(f"'{k}': [{', '.join(str(x) for x in v)}]" for k, v in sorted(layout_table(data).items()))
    source = open(PAUSE_SOURCE, encoding='utf-8').read().replace('FGUY_HUD_SCALES', '{' + table + '}')
    p.replace(o, 0, as2c.compile_source(source))
    # Online the HUD's menus (death menu, character / weapon screens) take no mouse clicks: a click lands on
    # the button on this PC's screen only (each PC shows its own quarter of the 4-way HUD), so a player's
    # menu went on on their own PC and nowhere else - out of sync. Keys / buttons go through the match's
    # input on every PC alike (Enter: continue, X: character, Y: customize).
    no_mouse = (A.Code(A.external('FGUYMOD_OnlineMatch'), A.pushes(True), A.op('Equals2'), A.op('Not')).if_true('offline') +
                A.Code(A.pushes(False), A.op('Return')).label('offline'))
    insert_at_start(p, data, actions, 'NeedMouseControls', lambda r: no_mouse)
    # ...so a keyboard player online sees which keys do what on the death menu (its buttons show no keys in
    # keyboard mode - they're for the mouse there): pause.as2js FGUYDeathHints.
    try:   # (the other HUD movie, PLYW, has no death menu)
        insert_at_start(p, data, actions, 'OnDeathMenuRefreshText',
                        lambda r: A.Code(A.call('FGUYDeathHints', Reg(r['playerIndex']))))
    except KeyError:
        pass
    #     (and on the change character / change weapons screens: FGUYNavHints)
    for name, which in (('OnCharacterSelectRefreshText', 0), ('OnCharacterCustomizeRefreshText', 1)):
        try:
            insert_at_start(p, data, actions, name,
                            lambda r, which=which: A.Code(A.call('FGUYNavHints', Reg(r['playerIndex']), which)))
        except KeyError:
            pass
    # The game hands the HUD real viewport numbers in many places (and the HUD stores them), so they
    # stay real everywhere; only the three helpers that turn a viewport into HUD objects translate
    # (each is always called with a real number, so every path translates exactly once).
    debug = '--debug' in sys.argv
    for name in ('GetObjectPath', 'IsViewportValid', 'UpdateViewportFrameMapping'):
        if debug and name == 'UpdateViewportFrameMapping':
            insert_at_start(p, data, actions, name, lambda r: viewport(r) + A.Code(A.external(
                'FGUYMOD_HudDebug', 'frame-map vp/player/list', Reg(r['ViewportIndex']), Reg(r['PlayerIndex']),
                A.member(A.getvar('DisplayList'), 'length')), A.op('Pop')))
        elif debug and name == 'GetObjectPath':
            insert_at_start(p, data, actions, name, lambda r: viewport(r) + A.Code(A.external(
                'FGUYMOD_HudDebug', 'path vp/player/mode', Reg(r['ViewportIndex']), Reg(r['PlayerIndex']),
                A.getvar('SplitScreenMode')), A.op('Pop')))
        else:
            insert_at_start(p, data, actions, name, viewport)
    if debug:
        insert_at_start(p, data, actions, 'SetHeadIcon', lambda r: A.Code(A.external(
            'FGUYMOD_HudDebug', 'head icon player', Reg(r['playerIndex'])), A.op('Pop')))
        insert_at_start(p, data, actions, 'SetAmmo', lambda r: A.Code(A.external(
            'FGUYMOD_HudDebug', 'ammo player/slot/count', Reg(r['playerIndex']), Reg(r['slot']), Reg(r['ammoCount'])),
            A.op('Pop')))
        insert_at_start(p, data, actions, 'BuildDisplayList', lambda r: A.Code(A.external(
            'FGUYMOD_HudDebug', 'build list mode/frame', A.getvar('SplitScreenMode'), A.getvar('SplitScreenFrame')),
            A.op('Pop')))
    p.fix_top_level_jumps(actions)
    assert struct.unpack_from('<H', data, tag)[0] & 0x3f == 0x3f, 'expected long DoAction header'
    p.fix_length_u32(tag + 2, start, end)
    p.fix_length_u32(4, 0, len(data))
    return p


def hud_movies(game_dir):
    seen = {}
    for f in glob.glob(os.path.join(game_dir, 'FGUY', '**', '*.ho'), recursive=True):
        ho = open(f, 'rb').read()
        for m in re.finditer(rb'GFX[\x08-\x0c]', ho):
            o = m.start()
            size = struct.unpack_from('<I', ho, o + 4)[0]
            blob = ho[o:o + size]
            if b'SetSplitScreenModeInternal' in blob and b'SetPlayerViewportInternal' in blob:
                seen.setdefault(zlib.crc32(blob), (blob, os.path.basename(f)))
    return seen


if __name__ == '__main__':
    os.makedirs(OUT_DIR, exist_ok=True)
    for crc, (data, where) in hud_movies(sys.argv[1]).items():
        p = build(data)
        name = f'hud_{crc:08x}.fgpatch'
        open(os.path.join(OUT_DIR, name), 'wb').write(p.to_fgpatch())
        print(f'{where}: hud {len(data)} bytes crc {crc:08x} -> {len(p.apply())} bytes, {len(p.edits)} edits -> {name}')
