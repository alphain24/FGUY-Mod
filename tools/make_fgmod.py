r"""Packs a mod folder into one file: make_fgmod.py <mod folder> [<out.fgmod>]

The whole mod - its mod.ini and its folders (skins\, emotes\, ui\, files\...) - in <folder name>.fgmod, which goes in
FGUYMod\mods\ as it is (src/loader/modloader.cpp unpacks it at start). The zip format, every entry stored (the mod reads
them as they are), in name order with a fixed time: the same folder always makes the same file.
"""
import os
import sys
import zipfile


def pack(folder, out=None):
    folder = os.path.abspath(folder)
    out = out or folder.rstrip('\\/') + '.fgmod'
    if not os.path.isfile(os.path.join(folder, 'mod.ini')):
        raise SystemExit(f'{folder} has no mod.ini')
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
    out, n = pack(sys.argv[1], sys.argv[2] if len(sys.argv) > 2 else None)
    print(f'{out}: {n} files, {os.path.getsize(out):,} bytes')
