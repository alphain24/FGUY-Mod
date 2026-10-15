"""The mouse for the game's own menus in an online match (the end of the match's RETRY / MAIN MENU, the pause menu,
their yes / no questions) - ActionScript shared by make_results_patch.py and make_pause_patch.py.

Online the game feeds its menus no mouse, and a click straight on a button would happen on this PC only - out of
sync. So the menu's buttons go to the loader instead (FGUYMOD_MenuMouse: where each one is on the screen, in 1/10000
of it, and which one is chosen now): pointing at one moves the choice there with Up / Down (a row of buttons: Left /
Right), a click moves it and presses Enter - keys in the match's input, the same tick on every PC
(src/loader/onlinepause.cpp). A page laid out as a grid (the mission select) stays keys only: the steps there aren't
one per button. The Windows mouse pointer shows meanwhile (the game's own is hidden online: make_cursor_patch.py).

FGUYMenuMouseStart(who) once the menu shows: who 'pause' (only the player who paused drives that menu) or 'results'.
"""

SOURCE = r'''
function FGUYMenuMouseStart(who) {
    if (flash.external.ExternalInterface.call('FGUYMOD_OnlineMatch') != true) return;
    _root.FGUYMenuWho = who;
    if (_root.FGUYMenuMC != undefined) return;
    var mc = _root.createEmptyMovieClip('FGUYMenuMC', 30500);
    mc.onEnterFrame = FGUYMenuMousePoll;
}

function FGUYMenuShown(mc) {
    while (mc != undefined && mc != _root) {
        if (!mc._visible) return false;
        mc = mc._parent;
    }
    return true;
}

// The buttons' boxes on the screen: a column (kind 2) or a row (kind 3) of them; a grid: none (keys only).
function FGUYMenuBoxes(list, cur, rx, ry, rw, rh) {
    var bs = [];
    for (var i = 0; i < list.length; i++) {
        var b = list[i];
        if (b == undefined || b == null || !FGUYMenuShown(b)) continue;
        var h = b.hitBox != undefined ? b.hitBox : b;
        var bb = h.getBounds(_root);
        if (bb.xMax <= bb.xMin || bb.yMax <= bb.yMin) continue;
        bs.push({i: b.buttonIndex != undefined ? b.buttonIndex : i, b: bb});
    }
    if (bs.length == 0) return '';
    var col = true, row = true;
    for (var k = 1; k < bs.length; k++) {
        var a = bs[k - 1].b, c = bs[k].b;
        if (Math.abs(a.xMin - c.xMin) > 4 && Math.abs((a.xMin + a.xMax) - (c.xMin + c.xMax)) > 8) col = false;
        if (Math.abs((a.yMin + a.yMax) - (c.yMin + c.yMax)) > 8) row = false;
    }
    if (!col && !row) return '';
    var kind = col ? 2 : 3, out = '';
    for (var j = 0; j < bs.length; j++) {
        var e = bs[j].b;
        out = out + Math.round((e.xMin - rx) / rw * 10000) + ',' + Math.round((e.yMin - ry) / rh * 10000) + ',' +
              Math.round((e.xMax - rx) / rw * 10000) + ',' + Math.round((e.yMax - ry) / rh * 10000) + ',13,' + kind + ',' +
              bs[j].i + ',' + cur + ';';
    }
    return out;
}

function FGUYMenuMousePoll() {
    var vr = Stage.visibleRect;
    var rx = 0, ry = 0, rw = 960, rh = 720;
    if (vr) { rx = vr.x; ry = vr.y; rw = vr.width; rh = vr.height; }
    var boxes = '';
    var d = ActionScript.dialog.Dialog._instance;
    if (d != undefined && d != null && d._visible && d._btns != undefined && d._btns.length > 0)   // a yes / no question
        boxes = FGUYMenuBoxes(d._btns, d._focusIndex, rx, ry, rw, rh);
    else if (buttonReferences != undefined && buttonReferences != null)
        boxes = FGUYMenuBoxes(buttonReferences, CurBtn, rx, ry, rw, rh);
    flash.external.ExternalInterface.call('FGUYMOD_MenuMouse', boxes, _root.FGUYMenuWho);
}
'''
