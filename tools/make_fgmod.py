r"""Packs a mod folder into one file: make_fgmod.py <mod folder> [<out.fgmod>]

The whole mod - its mod.ini and its folders (skins\, emotes\, ui\, files\...) - in <folder name>.fgmod, which goes in
FGUYMod\mods\ as it is (src/loader/modloader.cpp unpacks it at start). The zip format, every entry stored (the mod reads
them as they are), in name order with a fixed time: the same folder always makes the same file.

No command line needed: double-click this file, drag your mod's folder into the window, press Enter (or drop the folder
onto this file). The Skin Studio's "Save as mod" makes one too.
"""
import os
import sys
import zipfile


def pack(folder, out=None):
    folder = os.path.abspath(folder.replace('﻿', '').strip().strip('"\'').strip())   # (as dragged in: quoted)
    out = out or folder.rstrip('\\/') + '.fgmod'
    if not os.path.isdir(folder):
        raise SystemExit(f'{folder} is not a folder')
    if not os.path.isfile(os.path.join(folder, 'mod.ini')):
        raise SystemExit(f'{folder} has no mod.ini - a mod folder needs one (see docs/mod-structure.md)')
    files = []
    for root, dirs, names in os.walk(folder):
        dirs[:] = sorted(d for d in dirs if not d.startswith('.'))
        for n in sorted(names):
            if not n.startswith('.'):
                files.append(os.path.relpath(os.path.join(root, n), folder).replace('\\', '/'))
    with zipfile.ZipFile(out, 'w', zipfile.ZIP_STORED) as z:
        for rel in sorted(files):
            info = zipfile.ZipInfo(rel, date_time=(2026, 1, 1, 0, 0, 0))
            info.compress_type = zipfile.ZIP_STORED
            with open(os.path.join(folder, rel), 'rb') as f:
                z.writestr(info, f.read())
    return out, len(files)


if __name__ == '__main__':
    asked = len(sys.argv) < 2   # (double-clicked: ask, and wait before the window closes)
    try:
        folder = sys.argv[1] if not asked else input("Drag your mod's folder into this window, then press Enter:\n> ")
        out, n = pack(folder, sys.argv[2] if len(sys.argv) > 2 else None)
        print(f'Made {out}: {n} files, {os.path.getsize(out):,} bytes - put it in the game\'s FGUYMod\\mods folder.')
    except SystemExit as e:
        print(e)
    if asked:
        input('Press Enter to close.')
