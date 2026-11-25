// Pictures a model can come with that a browser can't show by itself - .tga (Targa: models made for games, Blender's
// exports) and .dds (DirectDraw Surface: DXT1 / DXT3 / DXT5 compressed, or plain 24 / 32-bit) - turned into PNGs before
// the model is read, so whichever loader asks for "skin.tga" gets a picture it can use.
//   await toPng('skin.tga', bytes)  -> PNG bytes (null: not a picture this reads)
import {TGALoader} from 'three/addons/loaders/TGALoader.js';

export const NEEDS_TURNING = /\.(tga|dds)$/i;

export async function toPng(name, bytes) {
  let img = null;
  try {
    if (/\.tga$/i.test(name)) img = readTga(bytes);
    else if (/\.dds$/i.test(name)) img = readDds(bytes);
  } catch (err) {
    console.warn('pictures: ' + name + ': ' + err.message);
    return null;
  }
  if (!img) return null;
  const c = document.createElement('canvas');
  c.width = img.width; c.height = img.height;
  c.getContext('2d').putImageData(new ImageData(img.rgba, img.width, img.height), 0, 0);
  const blob = await new Promise(r => c.toBlob(r, 'image/png'));
  return new Uint8Array(await blob.arrayBuffer());
}

// ---- .tga: three.js's own reader (every Targa kind: colour-mapped, true colour, grey, run-length) -------------------
// Its rows come out top first, whichever corner the file starts from (checked: a top-red, bottom-blue picture, both
// plain and run-length).
function readTga(bytes) {
  const t = new TGALoader().parse(bytes.buffer.slice(bytes.byteOffset, bytes.byteOffset + bytes.byteLength));
  return {width: t.width, height: t.height, rgba: new Uint8ClampedArray(t.data.buffer, t.data.byteOffset, t.width * t.height * 4)};
}

// ---- .dds: the first (biggest) picture in it -------------------------------------------------------------------------
function readDds(bytes) {
  const dv = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength);
  if (dv.getUint32(0, true) !== 0x20534444) throw new Error('not a .dds');   // "DDS "
  const h = dv.getUint32(12, true), w = dv.getUint32(16, true);
  const pfFlags = dv.getUint32(80, true), four = String.fromCharCode(...bytes.subarray(84, 88));
  let at = 128, kind = null;
  if (pfFlags & 0x4) {   // (compressed: a FourCC)
    if (four === 'DXT1') kind = 'bc1';
    else if (four === 'DXT2' || four === 'DXT3') kind = 'bc2';
    else if (four === 'DXT4' || four === 'DXT5') kind = 'bc3';
    else if (four === 'DX10') {
      const fmt = dv.getUint32(128, true);
      at = 148;
      if (fmt >= 70 && fmt <= 72) kind = 'bc1';
      else if (fmt >= 73 && fmt <= 75) kind = 'bc2';
      else if (fmt >= 76 && fmt <= 78) kind = 'bc3';
      else if (fmt === 87 || fmt === 91) kind = 'bgra';
      else if (fmt === 28 || fmt === 29) kind = 'rgba';
      else throw new Error('DX10 format ' + fmt + ' not read');
    } else throw new Error('compression ' + four + ' not read');
  }
  const rgba = new Uint8ClampedArray(w * h * 4);
  if (kind === 'bc1' || kind === 'bc2' || kind === 'bc3') {
    const blockBytes = kind === 'bc1' ? 8 : 16;
    const bw = Math.max(1, (w + 3) >> 2), bh = Math.max(1, (h + 3) >> 2);
    const color = new Uint8Array(16 * 4), alpha = new Uint8Array(16);
    for (let by = 0; by < bh; by++) for (let bx = 0; bx < bw; bx++) {
      const o = at + (by * bw + bx) * blockBytes;
      if (kind === 'bc1') { colorBlock(dv, o, color, true); for (let k = 0; k < 16; k++) alpha[k] = color[k * 4 + 3]; }
      else {
        if (kind === 'bc2') for (let k = 0; k < 16; k++) alpha[k] = ((dv.getUint8(o + (k >> 1)) >> ((k & 1) * 4)) & 15) * 17;
        else alphaBlock(dv, o, alpha);
        colorBlock(dv, o + 8, color, false);
      }
      for (let k = 0; k < 16; k++) {
        const x = bx * 4 + (k & 3), y = by * 4 + (k >> 2);
        if (x >= w || y >= h) continue;
        const p = (y * w + x) * 4;
        rgba[p] = color[k * 4]; rgba[p + 1] = color[k * 4 + 1]; rgba[p + 2] = color[k * 4 + 2]; rgba[p + 3] = alpha[k];
      }
    }
  } else {   // (not compressed: by its bit masks - or a DX10 8-bit RGBA / BGRA)
    let bits = dv.getUint32(88, true), masks = [92, 96, 100, 104].map(k => dv.getUint32(k, true));
    if (kind === 'bgra') { bits = 32; masks = [0xff0000, 0xff00, 0xff, 0xff000000]; }
    if (kind === 'rgba') { bits = 32; masks = [0xff, 0xff00, 0xff0000, 0xff000000]; }
    if (!(pfFlags & 0x1)) masks[3] = 0;   // (no alpha in it)
    const step = bits >> 3;
    if (step < 2 || step > 4) throw new Error(bits + '-bit pixels not read');
    const shift = m => { let s = 0; if (!m) return 0; while (!((m >>> s) & 1)) s++; return s; };
    const sh = masks.map(shift), max = masks.map((m, i) => (m >>> sh[i]) || 1);
    const pitch = dv.getUint32(20, true) && (dv.getUint32(8, true) & 0x8) ? dv.getUint32(20, true) : w * step;
    for (let y = 0; y < h; y++) for (let x = 0; x < w; x++) {
      const o = at + y * pitch + x * step;
      let v = 0;
      for (let b = 0; b < step; b++) v |= dv.getUint8(o + b) << (8 * b);
      v >>>= 0;
      const p = (y * w + x) * 4;
      for (let c = 0; c < 3; c++) rgba[p + c] = masks[c] ? Math.round((((v & masks[c]) >>> 0) >>> sh[c]) * 255 / max[c]) : 0;
      rgba[p + 3] = masks[3] ? Math.round((((v & masks[3]) >>> 0) >>> sh[3]) * 255 / max[3]) : 255;
    }
  }
  return {width: w, height: h, rgba};
}

// a 4x4 block's colours: two 5:6:5 ends and 2-bit picks (DXT1 with the first end not bigger: three colours and clear)
function colorBlock(dv, o, out, dxt1) {
  const c0 = dv.getUint16(o, true), c1 = dv.getUint16(o + 2, true), bits = dv.getUint32(o + 4, true);
  const rgb = c => [((c >> 11) & 31) * 255 / 31, ((c >> 5) & 63) * 255 / 63, (c & 31) * 255 / 31];
  const a = rgb(c0), b = rgb(c1), pal = [[...a, 255], [...b, 255]];
  if (c0 > c1 || !dxt1) {
    pal.push([0, 1, 2].map(i => (2 * a[i] + b[i]) / 3).concat(255), [0, 1, 2].map(i => (a[i] + 2 * b[i]) / 3).concat(255));
  } else {
    pal.push([0, 1, 2].map(i => (a[i] + b[i]) / 2).concat(255), [0, 0, 0, 0]);
  }
  for (let k = 0; k < 16; k++) {
    const c = pal[(bits >>> (2 * k)) & 3];
    out[k * 4] = c[0]; out[k * 4 + 1] = c[1]; out[k * 4 + 2] = c[2]; out[k * 4 + 3] = c[3];
  }
}
// a DXT5 block's alpha: two ends and 3-bit picks
function alphaBlock(dv, o, out) {
  const a0 = dv.getUint8(o), a1 = dv.getUint8(o + 1), pal = [a0, a1];
  if (a0 > a1) for (let i = 1; i < 7; i++) pal.push(((7 - i) * a0 + i * a1) / 7);
  else { for (let i = 1; i < 5; i++) pal.push(((5 - i) * a0 + i * a1) / 5); pal.push(0, 255); }
  let bits = 0n;
  for (let i = 0; i < 6; i++) bits |= BigInt(dv.getUint8(o + 2 + i)) << BigInt(8 * i);
  for (let k = 0; k < 16; k++) out[k] = pal[Number((bits >> BigInt(3 * k)) & 7n)];
}
