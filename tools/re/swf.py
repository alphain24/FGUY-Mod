"""Minimal SWF / Scaleform GFX tag reader. Usage: swf.py <file.gfx> [--tags]"""
import struct
import sys

TAG_NAMES = {0: 'End', 1: 'ShowFrame', 9: 'SetBackgroundColor', 12: 'DoAction', 26: 'PlaceObject2',
             39: 'DefineSprite', 43: 'FrameLabel', 56: 'ExportAssets', 57: 'ImportAssets',
             59: 'DoInitAction', 69: 'FileAttributes', 70: 'PlaceObject3', 76: 'SymbolClass',
             82: 'DoABC', 86: 'DefineSceneAndFrameLabelData', 37: 'DefineEditText', 11: 'DefineText',
             1000: 'GFX_ExporterInfo', 1001: 'GFX_FontTextureInfo', 1002: 'GFX_DefineExternalImage',
             1003: 'GFX_FontTextureInfo2', 1004: 'GFX_DefineExternalGradient', 1005: 'GFX_DefineGradientMap',
             1006: 'GFX_DefineCompactedFont', 1007: 'GFX_DefineExternalSound', 1008: 'GFX_DefineExternalStreamSound',
             1009: 'GFX_DefineSubImage', 1010: 'GFX_DefineExternalImage2'}


def header_end(data):
    nbits = data[8] >> 3
    rect_bytes = (5 + 4 * nbits + 7) // 8
    return 8 + rect_bytes + 4  # + frame rate, frame count


def tags(data, start, end):
    """Yield (code, body_offset, body_length, tag_offset)."""
    o = start
    while o + 2 <= end:
        cl = struct.unpack_from('<H', data, o)[0]
        code, ln = cl >> 6, cl & 0x3f
        hdr = 2
        if ln == 0x3f:
            ln = struct.unpack_from('<I', data, o + 2)[0]
            hdr = 6
        yield code, o + hdr, ln, o
        o += hdr + ln
        if code == 0:
            break


def walk(data, start, end, depth=0, out=None):
    out = [] if out is None else out
    for code, body, ln, to in tags(data, start, end):
        out.append((depth, code, body, ln, to))
        if code == 39:  # DefineSprite: id(2) frames(2) then nested tags
            walk(data, body + 4, body + ln, depth + 1, out)
    return out


if __name__ == '__main__':
    data = open(sys.argv[1], 'rb').read()
    print('signature', data[:3], 'version', data[3], 'length', struct.unpack_from('<I', data, 4)[0])
    all_tags = walk(data, header_end(data), len(data))
    from collections import Counter
    c = Counter(TAG_NAMES.get(t[1], t[1]) for t in all_tags)
    for name, n in c.most_common():
        print(f'{n:6} {name}')
    if '--tags' in sys.argv:
        for depth, code, body, ln, to in all_tags:
            print('  ' * depth + f'{TAG_NAMES.get(code, code)} @ {to:#x} len {ln}')
