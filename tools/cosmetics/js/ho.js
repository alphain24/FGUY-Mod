// Reads the game's .ho packages (Heavy Iron's GoodEngine archives, little endian on PC) straight from the player's own
// file - only the parts asked for (File.slice), nothing uploaded. Format: heavyironmodding.org/wiki/GoodEngine/HO_(File_Format);
// the asset names table walked as the PC build has it (tools/re/ho.py is the research version of this).

export const T = {
  Model: 0x5247BB31, SkinGeometry: 0x66EC031B, SkeletonBlob: 0x9E3F4786, Texture: 0x2952081F,
  Material: 0x189F55BF, RawBlob: 0xBDE21A8D,
};

const u32 = (dv, o) => dv.getUint32(o, true);

export class Asset {
  constructor(file, base, dv, o) {
    this.file = file;
    this.sizePadded = u32(dv, o);
    this.offset = base + u32(dv, o + 4);
    this.size = u32(dv, o + 8);
    this.lo = u32(dv, o + 0x10);
    this.hi = u32(dv, o + 0x14);
    this.type = u32(dv, o + 0x18);
    this.name = '';
  }
  get key() { return Asset.key(this.lo, this.hi); }
  static key(lo, hi) { return lo.toString(16) + ':' + hi.toString(16); }
  async bytes() {
    if (!this._bytes) this._bytes = new Uint8Array(await this.file.slice(this.offset, this.offset + this.size).arrayBuffer());
    return this._bytes;
  }
}

async function read(file, start, length) {
  return new DataView(await file.slice(start, start + length).arrayBuffer());
}

export class Ho {
  static async open(file) {
    const ho = new Ho();
    ho.file = file;
    const h = await read(file, 0, 0x800);
    if (u32(h, 0) !== 0x1a4c4548) throw new Error(file.name + " isn't a PC .ho package");   // "HEL\x1a"
    const sector = u32(h, 0x40);
    const mast = await read(file, u32(h, 0x44) * sector, 0x60);
    const sectStart = u32(mast, 0x20 + 0x1c) * sector;
    const th = await read(file, sectStart, 0x20);
    const count = u32(th, 4);
    const entries = await read(file, sectStart + 0x20, count * 0x40);
    ho.assets = [];
    const names = [];
    for (let i = 0; i < count; i++) {
      const e = i * 0x40;
      const base = u32(entries, e + 0x1c) * sector;
      const metaRec = u32(entries, e + 0x38);
      const meta = await read(file, sectStart + metaRec, 0x20);
      const magic = u32(meta, 0);
      if (magic === 0x444c5350) {   // "PSLD": the names
        names.push([base, u32(meta, 8), u32(meta, 0xc)]);
      } else if ((magic & 0xffffff) === 0x4c5350) {   // "PSL": the slices - the TOC one lists the assets
        const n = u32(meta, 8);
        const slices = await read(file, sectStart + metaRec + 0x10, n * 0x10);
        for (let s = 0; s < n; s++) {
          if (u32(slices, s * 0x10) !== 0) continue;
          const tocAt = base + u32(slices, s * 0x10 + 4);
          const tocHead = await read(file, tocAt, 0x20);
          const cnt = u32(tocHead, 0);
          const toc = await read(file, tocAt + 0x20, cnt * 0x20);
          for (let a = 0; a < cnt; a++) ho.assets.push(new Asset(file, base, toc, a * 0x20));
        }
      }
    }
    ho.byKey = new Map(ho.assets.map(a => [a.key, a]));
    for (const [base, cnt, offset] of names) await ho.readNames(base + offset, cnt);
    return ho;
  }

  // (each: the asset id, 0x20, a hash, -1, then the name, then 0x33 bytes to the next 0x20 / 0x40 boundary)
  async readNames(at, count) {
    const buf = new Uint8Array(await this.file.slice(at, at + count * 0x80 + 0x10000).arrayBuffer());
    const dv = new DataView(buf.buffer);
    let pos = 0;
    for (let i = 0; i < count && pos + 0x20 <= buf.length; i++) {
      const lo = u32(dv, pos), hi = u32(dv, pos + 4), rel = u32(dv, pos + 8);
      let end = pos + rel;
      while (end < buf.length && buf[end]) end++;
      const a = this.byKey.get(Asset.key(lo, hi));
      if (a) a.name = String.fromCharCode(...buf.subarray(pos + rel, end));
      let next = -1;
      for (const align of [0x20, 0x40, 0x10]) {
        const p = (end + 1 + align - 1) & ~(align - 1);
        if (p + 0x14 <= buf.length && u32(dv, p + 8) === 0x20 && u32(dv, p + 0x10) === 0xffffffff) { next = p; break; }
      }
      if (next < 0) break;
      pos = next;
    }
  }

  get(lo, hi) { return this.byKey.get(Asset.key(lo >>> 0, hi >>> 0)); }

  // the assets an asset's data points to (asset ids anywhere in it, on 4-byte boundaries)
  refs(bytes) {
    const dv = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength);
    const out = [];
    for (let i = 0; i + 8 <= bytes.length; i += 4) {
      const a = this.get(u32(dv, i), u32(dv, i + 4));
      if (a) out.push({at: i, asset: a});
    }
    return out;
  }
}
