"""Builds dist/FGUYMod/ui/menu_<crc>.fgpatch for the front-end menu movie (in MNUS.ho).

Changes:
  * Options > Screen > Display Mode gets a third choice, "Borderless".
  * The display-mode getter/setter ask the mod loader (FGUYMOD_GetDisplayMode / FGUYMOD_SetDisplayMode)
    so it can track borderless alongside the game's own windowed flag.
  * The Screen page's OK handler first calls FGUYMOD_ApplyDisplayMode.
  * A new Options > Screen row, Frame Rate (VSync / 30-240 FPS / Uncapped), backed by
    FGUYMOD_GetFrameRate / FGUYMOD_SetFrameRate.
  * Main Menu > ONLINE: the online lobby page (src/loader/online.cpp has the logic). It reuses the
    Options > Screen page's frame: while FGUYOnline is set, that page's events (On<page>Enter,
    ButtonHover, ButtonPressed, Exit) go to our OnOnline* functions instead.

How list rows work in this menu: a row named X takes its choices from ButtonListStrings[
ButtonIndexMapping[X]], reads its value with GetXValue() and reports changes to OnXValueChanged(v).

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
from gfxpatch import (Patch, Reg, action, constant_pool, define_function, define_function2,  # noqa: E402
                      external_call, function_at, parse_actions, push, push_records, record_text,
                      root_doaction_containing)
import as2asm as A  # noqa: E402

HERE = os.path.dirname(os.path.abspath(__file__))
OUT_DIR = os.path.join(HERE, '..', '..', 'dist', 'FGUYMod', 'ui')
MOVIE_SIZE = 290635  # the front-end menu movie inside FGUY/Global_Scenes/MNUS.ho

# Frame Rate choices, in the order the loader expects (d3d9.cpp kFrameRates).
FRAME_RATES = ['VSync', '30 FPS', '60 FPS', '120 FPS', '144 FPS', '240 FPS', 'Uncapped']

# The ONLINE page: five rows (what the borrowed page's panel fits), Online1..Online5. The loader decides
# what each row is right now: FGUYMOD_OnlineLabel(row) / FGUYMOD_OnlineText(row) give its label and
# value, FGUYMOD_OnlinePress(row) handles A. Online3 is a list row (left/right browses the open games, or
# the players while in a lobby): its choices come from FGUYMOD_OnlineList(), '|' separated.
ONLINE_ROWS = ['Online1', 'Online2', 'Online3', 'Online4', 'Online5']
ONLINE_LIST_ROW = 'Online3'
ONLINE_LIST_INDEX = 100  # its entry in ButtonListStrings (the game's own lists use 0..21)


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


def find_action(actions, code, pred):
    for o, c, total in actions:
        if c == code and pred(o, total):
            return o, total
    raise LookupError(f'action {code:#x} not found')


def build(data):
    p = Patch(data)
    anchor = data.index(b'OnDisplayModeValueChanged')
    tag, start, end = root_doaction_containing(data, anchor)
    actions = parse_actions(data, start, end)
    pool = constant_pool(data, start)

    def texts(o):
        return [record_text(r, pool) for r in push_records(data, o)]

    # 1. Choices table (ButtonListStrings: one array of choices per list row).
    #    Original: Push 'ButtonListStrings', pool'=MNU_FULLSCREEN', pool'=MNU_WINDOWED', 2 ; InitArray ...
    #    InitArray pops the top value as element 0, so later elements are pushed deeper. The Display
    #    Mode array (row 20) is the deepest, right after 'ButtonListStrings'.
    #    a) Display Mode gets a third choice, Borderless.
    #    b) A new row 21, Frame Rate, is pushed deeper still (between 'ButtonListStrings' and the
    #       Display Mode values), which means splitting this Push in two.
    o, total = find_action(actions, 0x96, lambda o, t: texts(o)[:4] ==
                           ['ButtonListStrings', '=MNU_FULLSCREEN', '=MNU_WINDOWED', None]
                           and push_records(data, o)[3][3] == 2)
    recs = push_records(data, o)
    first_len = recs[0][1]
    borderless = push('Borderless')[3:]                                  # the value record only
    rest_len = len(borderless) + (o + total - recs[1][0])                # Borderless + original values
    frame_rates = push(*reversed(FRAME_RATES), len(FRAME_RATES)) + action('InitArray')
    p.replace(o + 1, 2, struct.pack('<H', first_len))                    # first Push: just the name
    p.replace(o + 3 + first_len, 0, frame_rates + b'\x96' + struct.pack('<H', rest_len) + borderless)
    p.replace(recs[3][0] + 1, 4, struct.pack('<i', 3))                   # Display Mode count 2 -> 3
    #    c) The table's row count: Push 21 ; InitArray ; DefineLocal  ->  22
    idx = next(i for i, a in enumerate(actions) if a[0] == o)
    for k in range(idx + 1, len(actions) - 2):
        a, b, c = actions[k], actions[k + 1], actions[k + 2]
        if a[1] == 0x96 and b[1] == 0x42 and c[1] == 0x3C:
            r = push_records(data, a[0])
            assert len(r) == 1 and r[0][2] == 7 and r[0][3] == 21, r
            p.replace(r[0][0] + 1, 4, struct.pack('<i', 22))
            break
    else:
        sys.exit('choices table end not found')

    # 2. Row name -> table index (ButtonIndexMapping): add FrameRate: 21.
    o, total = find_action(actions, 0x96, lambda o, t: texts(o)[:1] == ['ButtonIndexMapping'])
    recs = push_records(data, o)
    assert texts(o)[-3] == 'DisplayMode' and recs[-2][3] == 20 and recs[-1][3] == 21
    extra = push('FrameRate', 21)[3:]
    p.replace(o + 1, 2, struct.pack('<H', total - 3 + len(extra)))
    p.replace(recs[-1][0], 0, extra)
    p.replace(recs[-1][0] + 1, 4, struct.pack('<i', 22))                 # pair count 21 -> 22

    # 3. GetDisplayModeValue() -> return Number(FGUYMOD_GetDisplayMode()); plus the Frame Rate row's
    #    getter and change handler, defined right after it.
    o, total = function_at(data, actions, 'GetDisplayModeValue')
    funcs = define_function('GetDisplayModeValue', [],
                            external_call('FGUYMOD_GetDisplayMode') + action('ToNumber') + action('Return'))
    funcs += define_function('GetFrameRateValue', [],
                             external_call('FGUYMOD_GetFrameRate') + action('ToNumber') + action('Return'))
    funcs += define_function2('OnFrameRateValueChanged', ['curValue'],
                              external_call('FGUYMOD_SetFrameRate', Reg(1)) + action('Pop'))
    funcs += online_functions()
    p.replace(o, total, funcs)

    # 4. OnDisplayModeValueChanged(curValue): replace 'needWindowed = (curValue == 0)' with
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

    # 5. OnOptionsScreenNextButtonPressed(): first let the loader apply the chosen mode. Switching
    #    between Windowed and Borderless doesn't change the game's own windowed flag, so the original
    #    code would skip GRFX_ChangeResolution.
    o, total = function_at(data, actions, 'OnOptionsScreenNextButtonPressed')
    code_start, code_end, size_field = function2_code(data, o)
    new_code = external_call('FGUYMOD_ApplyDisplayMode') + action('Pop')
    p.replace(code_start, 0, new_code)
    p.replace(size_field, 2, struct.pack('<H', code_end - code_start + len(new_code)))

    # 6. OnOptionsScreenEnter(): add a FRAME RATE row after Reticle (before the list is laid out with
    #    'Push r3, linkageID, ...'). Same shape as the original rows; r2 is the scratch register.
    o, total = function_at(data, actions, 'OnOptionsScreenEnter')
    code_start, code_end, size_field = function2_code(data, o)
    inner = parse_actions(data, code_start, code_end)
    at, _ = find_action(inner, 0x96, lambda q, t: texts(q)[:2] == [None, 'linkageID'])
    row = (push(0, 'Object') + action('NewObject') + action('StoreRegister', b'\x02') + action('Pop') +
           push(Reg(2), 'name', 'FrameRate') + action('SetMember') +
           push(Reg(2), 'text', 'FRAME RATE') + action('SetMember') +
           push(Reg(2), 1, Reg(3), 'push') + action('CallMethod') + action('Pop'))
    p.replace(at, 0, row)
    p.fix_top_level_jumps(inner)
    p.replace(size_field, 2, struct.pack('<H', code_end - code_start + p.delta_between(code_start, code_end)))

    # 7. OnOptionsScreenButtonHover(buttonStr): FrameRate is a list row too (shows arrows, creates the
    #    list). Insert 'if (r0 === "FrameRate") goto create' before the original fall-through Jump.
    o, total = function_at(data, actions, 'OnOptionsScreenButtonHover')
    code_start, code_end, size_field = function2_code(data, o)
    inner = parse_actions(data, code_start, code_end)
    jump_at = next(a for a, code, t in inner if code == 0x99)           # first Jump = "not a list row"
    check = push(Reg(0), 'FrameRate') + action('StrictEquals') + action('If', struct.pack('<h', 5))
    p.replace(jump_at, 0, check)                                         # If skips the 5-byte Jump
    p.fix_top_level_jumps(inner)
    p.replace(size_field, 2, struct.pack('<H', code_end - code_start + p.delta_between(code_start, code_end)))

    # 8. GetEventFuncName(Name, Event): while the ONLINE page is up, events of the page it borrows
    #    ('OptionsScreen') go to OnOnline<Event> instead. Inserted at the very start (r1 = Name).
    o, total = function_at(data, actions, 'GetEventFuncName')
    code_start, code_end, size_field = function2_code(data, o)
    redirect = (A.Code(A.getvar('FGUYOnline'), A.op('Not')).if_true('keep') +
                A.Code(A.pushes(Reg(1), 'OptionsScreen'), A.op('StrictEquals'), A.op('Not')).if_true('keep') +
                A.store(1, 'Online'))
    redirect.label('keep')
    new_code = redirect.assemble()
    p.replace(code_start, 0, new_code)
    p.replace(size_field, 2, struct.pack('<H', code_end - code_start + len(new_code)))

    # 9. OnHomeEnter(): FGUYOnline = false first; an ONLINE button right after MULTIPLAYER (before the
    #    'demoType' check that adds CHALLENGE MODE). Same shape as the original buttons (r2 scratch,
    #    r3 the button array).
    o, total = function_at(data, actions, 'OnHomeEnter')
    code_start, code_end, size_field = function2_code(data, o)
    inner = parse_actions(data, code_start, code_end)
    at, _ = find_action(inner, 0x96, lambda q, t: texts(q) == ['demoType'])
    button = (A.store(2, A.new_object('Object')) + A.setmember(Reg(2), 'name', 'Online') +
              A.setmember(Reg(2), 'text', 'ONLINE') + A.method(Reg(3), 'push', Reg(2))).assemble()
    p.replace(code_start, 0, A.setvar('FGUYOnline', False).assemble())
    p.replace(at, 0, button)
    p.fix_top_level_jumps(inner)
    p.replace(size_field, 2, struct.pack('<H', code_end - code_start + p.delta_between(code_start, code_end)))

    # Fix-ups: top-level jumps across edits, DoAction tag length, file length in the GFX header.
    p.fix_top_level_jumps(actions)
    assert struct.unpack_from('<H', data, tag)[0] & 0x3f == 0x3f, 'expected long DoAction header'
    p.fix_length_u32(tag + 2, start, end)
    p.fix_length_u32(4, 0, len(data))
    return p


def online_functions():
    """The ONLINE page (see ONLINE_ROWS). r1 = _root in all of them (the game's own convention)."""
    ext = A.external
    container = A.member(A.ROOT, 'subMenu', 'buttonContainer')

    # Main Menu > ONLINE
    select = A.Code(A.setvar('FGUYOnline', True), A.call('PauseTVSound'), A.call('PlayOptionsSubMenuMusic'),
                    A.call('GotoPage', 'OptionsScreen'))

    # Page enter: connect, build the rows, start refreshing.
    enter = A.Code(ext('FGUYMOD_OnlineEnter'), A.op('Pop'), A.setvar('CurButtonListFetchMode', 'FGUYMOD'))
    enter += A.setmember(A.member(A.ROOT, 'Title'), 'titleText', 'ONLINE')
    enter += A.setmember(A.getvar('ButtonIndexMapping'), ONLINE_LIST_ROW, ONLINE_LIST_INDEX)
    enter += A.setmember(A.getvar('ButtonListStrings'), ONLINE_LIST_INDEX, A.new_object('Array', ' '))
    enter += A.store(3, A.new_object('Array'))
    for row in ONLINE_ROWS:
        enter += (A.store(2, A.new_object('Object')) + A.setmember(Reg(2), 'name', row) +
                  A.setmember(Reg(2), 'text', ' ') + A.method(Reg(3), 'push', Reg(2)))
    enter += A.setmember(Reg(3), 'linkageID', A.getvar('SCROLLER_LINKAGE_ID'))
    enter += A.setmember(Reg(3), 'buttonHeight', A.getvar('SCROLLER_BUTTON_HEIGHT'))
    enter += A.setmember(Reg(3), 'align', A.getvar('SCROLLER_ALIGN'))
    enter += A.call('GenerateButtons', container, Reg(3), 0, True)
    enter += A.call('FGUYOnlineRefresh')
    enter += A.setvar('FGUYOnlineTimer', A.callv('setInterval', A.getvar('FGUYOnlineRefresh'), 150))

    # Refresh: write every row's label and value straight into its text fields (the label is
    # button.text / button.text2 as GenerateButtons sets it, the value button.textbox.animText.textbox).
    # The list row's choices also go to ButtonListStrings so left/right can browse them.
    refresh = A.Code(A.store(2, container), A.Code(A.val(Reg(2)), A.op('Not')).if_true('done'))
    for row in ONLINE_ROWS:
        skip = 'skip_' + row
        refresh += A.store(3, A.member(Reg(2), row, 'button'))
        refresh += A.Code(A.val(Reg(3)), A.op('Not')).if_true(skip)
        refresh += A.store(4, A.callv('String', ext('FGUYMOD_OnlineLabel', row)))
        refresh += A.setmember(A.member(Reg(3), 'text'), 'htmlText', Reg(4))
        refresh += A.setmember(A.member(Reg(3), 'text2'), 'htmlText', Reg(4))
        refresh += A.setmember(A.member(Reg(3), 'textbox', 'animText', 'textbox'), 'htmlText',
                               A.callv('String', ext('FGUYMOD_OnlineText', row)))
        refresh.label(skip)
    refresh += A.setmember(A.getvar('ButtonListStrings'), ONLINE_LIST_INDEX,
                           A.methodv(A.callv('String', ext('FGUYMOD_OnlineList')), 'split', '|'))
    refresh += A.Code(A.pushes(A.getvar('CurButtonList'), ONLINE_LIST_ROW), A.op('StrictEquals'),
                      A.op('Not')).if_true('done')
    refresh += A.setvar('NumListBtns', A.member(A.getvar('ButtonListStrings'), ONLINE_LIST_INDEX, 'length'))
    refresh.label('done')

    exit_ = A.Code(A.call('clearInterval', A.getvar('FGUYOnlineTimer')), ext('FGUYMOD_OnlineExit'), A.op('Pop'),
                   A.setvar('FGUYOnline', False))

    # Hover: the list row (left/right browses).
    hover = A.Code(A.pushes(Reg(2), ONLINE_LIST_ROW), A.op('StrictEquals'), A.op('Not')).if_true('done')
    hover += A.call('SetButtonListFetchMode', 'FGUYMOD') + A.call('CreateButtonList', 'FGUYMOD', A.getvar('SF_Invoke'))
    hover.label('done')

    pressed = A.Code(ext('FGUYMOD_OnlinePress', Reg(2)), A.op('Pop'))

    return (A.define_function2('OnOnlineSelect', [], select) + A.define_function2('OnOnlineEnter', [], enter) +
            A.define_function2('FGUYOnlineRefresh', [], refresh) + A.define_function2('OnOnlineExit', [], exit_) +
            A.define_function2('OnOnlineButtonHover', ['buttonStr'], hover) +
            A.define_function2('OnOnlineButtonPressed', ['buttonStr'], pressed))


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
