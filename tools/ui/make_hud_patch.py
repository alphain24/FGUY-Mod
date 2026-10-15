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
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 're'))
import swf  # noqa: E402
from placements import matrix as placement_matrix  # noqa: E402

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


def read_rect(data, o):
    """(xmin, xmax, ymin, ymax in twips, bits per value, byte length) of the SWF RECT at o."""
    nb = data[o] >> 3
    n = (5 + 4 * nb + 7) // 8
    bits = int.from_bytes(data[o:o + n], 'big')
    vals = []
    for i in range(4):
        v = (bits >> (n * 8 - 5 - nb * (i + 1))) & ((1 << nb) - 1)
        vals.append(v - (1 << nb) if v >> (nb - 1) else v)
    return vals, nb, n


def write_rect(vals, nb, n):
    bits = nb
    for v in vals:
        assert -(1 << (nb - 1)) <= v < (1 << (nb - 1)), 'rect value too big for its bits'
        bits = (bits << nb) | (v & ((1 << nb) - 1))
    return (bits << (n * 8 - 5 - 4 * nb)).to_bytes(n, 'big')


def placement_of(data, parent_id, char_id=None, name=None):
    """(sx, sy, tx, ty) of the first PlaceObject2 / 3 in sprite parent_id placing char_id / named name."""
    top = swf.header_end(data)
    for code, body, ln, to in swf.tags(data, top, len(data)):
        if code != 39 or struct.unpack_from('<H', data, body)[0] != parent_id:
            continue
        for c2, b2, l2, t2 in swf.tags(data, body + 4, body + ln):
            if c2 not in (26, 70) or not data[b2] & 0x04:
                continue
            o = b2 + (3 if c2 == 26 else 4)
            if c2 == 70 and data[b2 + 1] & 0x08:   # (PlaceObject3: a class name first)
                o = data.index(b'\0', o) + 1
            cid = None
            if data[b2] & 0x02:
                cid = struct.unpack_from('<H', data, o)[0]
                o += 2
            if (char_id is not None and cid == char_id) or (name is not None and name.encode() in data[b2:b2 + l2]):
                return placement_matrix(data, o)[0]
    raise KeyError((parent_id, char_id, name))


def center_ready_text(p, data):
    """Multiverse Madness' READY announcement (=MNU_READY: "PRESS [R] WHEN READY" for a keyboard player) is
    left-aligned in a wide box: the short controller text looked centred, the keyboard one ran off to the right of
    the screen - offline too. Centred instead, on the point the announcement grows from (ready_animGroup's origin:
    the screen's middle in the one-player layout), its box made even around that point."""
    top = swf.header_end(data)
    fields = [(body, ln, to) for depth, code, body, ln, to in swf.walk(data, top, len(data))
              if code == 37 and b'=MNU_READY' in data[body:body + ln] and b'align="left"' in data[body:body + ln]]
    for body, ln, to in fields:
        cid = struct.unpack_from('<H', data, body)[0]
        # the box's parent sprite (the one placing it), and the sprite placing that one as ready_animGroup
        holder = next(sid for sid in sprite_ids(data) if placed_in(data, sid, cid))
        group = next(sid for sid in sprite_ids(data) if placed_in(data, sid, holder))
        gsx, _, gtx, _ = placement_of(data, group, name='ready_animGroup')
        fsx, _, ftx, _ = placement_of(data, holder, char_id=cid)
        pivot = ((-gtx / gsx) - ftx) / fsx * 20   # in the box's own twips
        (x0, x1, y0, y1), nb, n = read_rect(data, body + 2)
        half = (x1 - x0) / 2
        p.replace(body + 2, n, write_rect([round(pivot - half), round(pivot + half), y0, y1], nb, n))
        o = body + 2 + n
        f1, f2 = data[o], data[o + 1]
        o += 2 + (4 if f1 & 0x01 else 0)
        if f2 & 0x80:
            o = data.index(b'\0', o) + 1
        o += (4 if f1 & 0x04 else 0) + (2 if f1 & 0x02 else 0)
        if f2 & 0x20:
            p.replace(o, 1, b'\x02')   # layout: centre
        h = data.index(b'align="left"', body, body + ln)
        p.replace(h, len(b'align="left"'), b'align="center"')
        assert struct.unpack_from('<H', data, to)[0] & 0x3f == 0x3f, 'expected a long DefineEditText header'
        p.fix_length_u32(to + 2, body, body + ln)
    return len(fields)


def sprite_ids(data):
    top = swf.header_end(data)
    return [struct.unpack_from('<H', data, body)[0] for code, body, ln, to in swf.tags(data, top, len(data)) if code == 39]


def placed_in(data, sprite_id, char_id):
    try:
        placement_of(data, sprite_id, char_id=char_id)
        return True
    except KeyError:
        return False


def build(data):
    p = Patch(data)
    anchor = data.index(b'SetSplitScreenModeInternal')
    tag, start, end = root_doaction_containing(data, anchor)
    actions = parse_actions(data, start, end)

    # (the second argument: does this HUD have a 4-way layout? The story's hudCampaign.gfx has none - only one and
    #  two players' layouts)
    has_4way = 1 if b'Icon_Lifebar_0_4' in data else 0

    def split_mode(r):  # ScreenFrame = ScreenMode = FGUYMOD_HudSplitMode(ScreenMode, has 4-way)
        frame, mode = r['ScreenFrame'], r['ScreenMode']
        return (A.store(mode, A.Code(A.external('FGUYMOD_HudSplitMode', Reg(mode), has_4way), A.op('ToNumber'))) +
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
        for name, where in (('OnCharacterCustomizeKeyDown', 1), ('OnDeathMenuKeyDown', 2)):
            try:   # (which keys reach the screens' own key handlers - pause.as2js FGUYKeyProbe)
                insert_at_start(p, data, actions, name,
                                lambda r, where=where: A.Code(A.call('FGUYKeyProbe', Reg(r['playerIndex']), where)))
            except KeyError:
                pass
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
    center_ready_text(p, data)
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
