"""Reads the game's .ho packages (Heavy Iron's GoodEngine archives - little endian on PC). Format from the Heavy Iron
Modding wiki (heavyironmodding.org/wiki/GoodEngine/HO_(File_Format)); asset type names from HoExtractor's list
(github.com/barncastle/HoExtractor, HoLib/Static/AssetType.cs).

  ho.py <file.ho> [type name or hex]          list the assets (id, type, size, name), optionally of one type
  ho.py <file.ho> --types                     how many assets of each type
  ho.py <file.ho> --dump <id hex> <out>       write one asset's data to a file
Nothing here writes game files; the output is for research only (never shared)."""
import struct
import sys
from collections import Counter

TYPES = {
    0x5247BB31: 'Model', 0x66EC031B: 'SkinGeometry', 0x9E3F4786: 'SkeletonBlob', 0x2952081F: 'Texture',
    0x189F55BF: 'Material', 0x0A208F1C: 'GenericShader', 0x1075999B: 'ShaderCodeBlob', 0x248863A9: 'VertexBufGPU',
    0x88095AB2: 'IndexBuffer', 0x51827566: 'VertexDecl', 0x86EF2978: 'StaticGeometry', 0xDE180D0E: 'StaticGeom',
    0xCF187818: 'ImmediateModel', 0x3206524B: 'ImmediateGeometry', 0x1A2BDCF1: 'ImmediateGeom',
    0x098E112C: 'CharacterAssets', 0x49C84BB8: 'FGUYPlayerTemplate', 0x6188B5F2: 'AnimationSet', 0x69DCEB73: 'AnimList',
    0xAD8343CD: 'AnimPackage', 0x9B14DD74: 'ComboAnimBlob', 0xF33D4358: 'BoneMask', 0xD836DA19: 'Player',
    0x714C4581: 'RenderMode', 0xBDE21A8D: 'RawBlob', 0x1A170D14: 'RawAsset', 0x4DB6973E: 'GEN_RawBlob',
    0x9DE475D0: 'WeaponAsset', 0x21AA26F6: 'ScaleformAsset', 0xFAC61192: 'UIDModelReference', 0x62671137: 'UIDReference',
    0xEB1FD67E: 'NPCGeneric', 0x26382C1F: 'NPCTemplate', 0xBA0466EB: 'CollisionMesh', 0x95D0CC15: 'ToonSettings',
    0x0A7BD900: 'TextureAnimationSettings', 0x2F2F3873: 'UVMovementSettings', 0xA0DB9C91: 'Effect', 0x52FE9CED: 'Camera',
    0x7E6B3037: 'FXParticleSystem', 0x9A0C1BAD: 'FXSpawn', 0x2F0B4FF7: 'Script', 0xB9FEA570: 'Scene',
    0x122BE3E3: 'Reference', 0x5E6449B0: 'SoundCue', 0x124EF085: 'SoundFX', 0xC9F2C7F9: 'SoundBankWrap',
    0x3FECFDEA: 'Light', 0xB604E800: 'LightKit', 0x524C73D8: 'Movie', 0x09D9D549: 'Icon', 0x8D9AB2D0: 'UI_Image',
}


class Asset:
    def __init__(self, f, base, e):
        self.size_padded, off, self.size, self.align, lo, hi, self.type, self.sub, self.flags = e
        self.f, self.offset, self.id = f, base + off, (hi << 32) | lo
        self.lo, self.hi, self.name = lo, hi, ''

    @property
    def type_name(self):
        return TYPES.get(self.type, '%08x' % self.type)

    def data(self):
        self.f.seek(self.offset)
        return self.f.read(self.size)


class Ho:
    def __init__(self, path):
        self.f = f = open(path, 'rb')
        h = f.read(0x800)
        assert h[:4] == b'HEL\x1a', 'not a little-endian .ho'
        self.sector = struct.unpack_from('<I', h, 0x40)[0]
        mast_sector = struct.unpack_from('<I', h, 0x44)[0]
        f.seek(mast_sector * self.sector)
        mast = f.read(0x20 + 0x40)
        assert mast[:4] == b'MAST'
        sect_start = struct.unpack_from('<I', mast, 0x20 + 0x1C)[0] * self.sector
        f.seek(sect_start)
        th = f.read(0x20)
        assert th[:4] == b'SECT', th[:4]
        count, _, first_string, string_size, first_meta, meta_size = struct.unpack_from('<6I', th, 4)
        entries = f.read(count * 0x40)
        self.assets = []
        self.sections = []
        for i in range(count):
            e = entries[i * 0x40:(i + 1) * 0x40]
            stype = e[:4][::-1].rstrip(b'\0').decode('latin1')
            start, size_disk = struct.unpack_from('<II', e, 0x1C)
            meta_rec = struct.unpack_from('<I', e, 0x38)[0]
            base = start * self.sector
            self.sections.append((stype, base, size_disk))
            f.seek(sect_start + meta_rec)
            magic = f.read(4)
            if magic[:3] == b'PSL' and magic[3:4] != b'D':
                msize, nslices, _ = struct.unpack('<3I', f.read(12))
                slices = [struct.unpack('<4I', f.read(16)) for _ in range(nslices)]
                for st, sstart, ssize, salign in slices:
                    if st != 0:
                        continue
                    f.seek(base + sstart)
                    n, brick = struct.unpack('<II', f.read(8))
                    f.read(24)
                    for _ in range(n):
                        self.assets.append(Asset(f, base, struct.unpack('<IIIIIIIhh', f.read(0x20))))
            elif magic == b'PSLD':
                self._names(f, base, sect_start + meta_rec)

    def _names(self, f, base, at):
        # (PC: "PSLD", its size, the entry count, the entries' offset in the section; each entry is 0x20 bytes - the
        #  asset id, 0x20, a hash, -1 - then the name, then 0x33 bytes to a 4-byte boundary)
        f.seek(at)
        _, size, count, offset = struct.unpack('<4sIII', f.read(16))
        f.seek(base + offset)
        d = f.read(count * 0x80 + 0x10000)
        byid = {a.id: a for a in self.assets}
        pos = 0
        for _ in range(count):
            if pos + 0x20 > len(d):
                break
            lo, hi, rel = struct.unpack_from('<III', d, pos)
            end = d.index(b'\0', pos + rel)
            name = d[pos + rel:end].decode('latin1', 'replace')
            a = byid.get((hi << 32) | lo)
            if a:
                a.name = name
            nxt = None   # (the next entry: on a 0x20 / 0x40 boundary - the one whose header looks right)
            for align in (0x20, 0x40, 0x10):
                p = (end + 1 + align - 1) & ~(align - 1)
                if p + 0x14 <= len(d) and struct.unpack_from('<I', d, p + 8)[0] == 0x20 and \
                        struct.unpack_from('<I', d, p + 0x10)[0] == 0xFFFFFFFF:
                    nxt = p
                    break
            if nxt is None:
                break
            pos = nxt


def main():
    ho = Ho(sys.argv[1])
    if len(sys.argv) > 2 and sys.argv[2] == '--types':
        c = Counter(a.type_name for a in ho.assets)
        for t, n in c.most_common():
            print(f'{n:6} {t}')
        return
    if len(sys.argv) > 2 and sys.argv[2] == '--dump':
        want = int(sys.argv[3], 16)
        a = next(a for a in ho.assets if a.id == want or a.lo == want)
        open(sys.argv[4], 'wb').write(a.data())
        print(f'{a.type_name} {a.size} bytes -> {sys.argv[4]}')
        return
    want = sys.argv[2] if len(sys.argv) > 2 else None
    for a in ho.assets:
        if want and a.type_name.lower() != want.lower() and '%08x' % a.type != want.lower():
            continue
        print(f'{a.lo:08x}:{a.hi:08x} {a.type_name:20} {a.size:9} flags {a.flags & 0xffff:04x} sub {a.sub:5}  {a.name}')
    print(len(ho.assets), 'assets in', len(ho.sections), 'sections', [s[0] for s in ho.sections])


if __name__ == '__main__':
    main()
