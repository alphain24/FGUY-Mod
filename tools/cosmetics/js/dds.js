// The game's textures are DDS files (DXT1 / DXT3 / DXT5, or uncompressed 32-bit). decodeDds -> {width, height, rgba}
// (the largest mip level), for showing and editing them on a canvas.

function rgb565(c) {
  return [((c >> 11) & 31) * 255 / 31, ((c >> 5) & 63) * 255 / 63, (c & 31) * 255 / 31];
}

function colorBlock(dv, o, out, x0, y0, w, h, dxt1) {
  const c0 = dv.getUint16(o, true), c1 = dv.getUint16(o + 2, true), bits = dv.getUint32(o + 4, true);
  const a = rgb565(c0), b = rgb565(c1);
  const pal = [[...a, 255], [...b, 255]];
  if (!dxt1 || c0 > c1) {
    pal.push([(2 * a[0] + b[0]) / 3, (2 * a[1] + b[1]) / 3, (2 * a[2] + b[2]) / 3, 255]);
    pal.push([(a[0] + 2 * b[0]) / 3, (a[1] + 2 * b[1]) / 3, (a[2] + 2 * b[2]) / 3, 255]);
  } else {
    pal.push([(a[0] + b[0]) / 2, (a[1] + b[1]) / 2, (a[2] + b[2]) / 2, 255]);
    pal.push([0, 0, 0, 0]);
  }
  for (let y = 0; y < 4; y++) for (let x = 0; x < 4; x++) {
    const px = x0 + x, py = y0 + y;
    if (px >= w || py >= h) continue;
    const p = pal[(bits >> (2 * (y * 4 + x))) & 3], i = (py * w + px) * 4;
    out[i] = p[0]; out[i + 1] = p[1]; out[i + 2] = p[2]; out[i + 3] = p[3];
  }
}

export function decodeDds(bytes) {
  const dv = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength);
  if (dv.getUint32(0, true) !== 0x20534444) throw new Error('not a DDS texture');
  const height = dv.getUint32(12, true), width = dv.getUint32(16, true);
  const pfFlags = dv.getUint32(80, true), fourCC = String.fromCharCode(...bytes.subarray(84, 88));
  const out = new Uint8ClampedArray(width * height * 4);
  let o = 128;
  if (pfFlags & 4) {   // compressed
    const dxt1 = fourCC === 'DXT1';
    if (!['DXT1', 'DXT3', 'DXT5'].includes(fourCC)) throw new Error('texture format ' + fourCC + ' not handled yet');
    for (let by = 0; by < height; by += 4) for (let bx = 0; bx < width; bx += 4) {
      let alpha = null;
      if (fourCC === 'DXT3') {
        alpha = [];
        for (let i = 0; i < 16; i++) alpha.push(((bytes[o + (i >> 1)] >> ((i & 1) * 4)) & 15) * 17);
        o += 8;
      } else if (fourCC === 'DXT5') {
        const a0 = bytes[o], a1 = bytes[o + 1];
        const pal = [a0, a1];
        if (a0 > a1) for (let i = 1; i < 7; i++) pal.push(((7 - i) * a0 + i * a1) / 7);
        else { for (let i = 1; i < 5; i++) pal.push(((5 - i) * a0 + i * a1) / 5); pal.push(0, 255); }
        let bits = 0n;
        for (let i = 0; i < 6; i++) bits |= BigInt(bytes[o + 2 + i]) << BigInt(8 * i);
        alpha = [];
        for (let i = 0; i < 16; i++) alpha.push(pal[Number((bits >> BigInt(3 * i)) & 7n)]);
        o += 8;
      }
      colorBlock(dv, o, out, bx, by, width, height, dxt1);
      o += 8;
      if (alpha) for (let i = 0; i < 16; i++) {
        const px = bx + (i & 3), py = by + (i >> 2);
        if (px < width && py < height) out[(py * width + px) * 4 + 3] = alpha[i];
      }
    }
  } else {   // uncompressed 32-bit (B, G, R, A in memory)
    for (let i = 0; i < width * height; i++, o += 4) {
      out[i * 4] = bytes[o + 2]; out[i * 4 + 1] = bytes[o + 1]; out[i * 4 + 2] = bytes[o]; out[i * 4 + 3] = bytes[o + 3];
    }
  }
  const mips = Math.max(1, dv.getUint32(28, true));
  return {width, height, rgba: out, format: (pfFlags & 4) ? fourCC : 'RGBA', mips};
}

// ---- encoding (the skin pack's game-ready copy: the same format and mip levels as the game's texture) -------------
function to565(r, g, b) { return ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3); }

function encodeColorBlock(px, out, o, dxt1) {   // px: 16 x [r, g, b, a]
  let lo = [255, 255, 255], hi = [0, 0, 0];
  for (const p of px) for (let k = 0; k < 3; k++) { lo[k] = Math.min(lo[k], p[k]); hi[k] = Math.max(hi[k], p[k]); }
  const inset = [0, 1, 2].map(k => (hi[k] - lo[k]) / 16);   // (a little in from the extremes: less banding)
  lo = lo.map((v, k) => Math.min(255, v + inset[k])); hi = hi.map((v, k) => Math.max(0, hi[k] - inset[k]));
  let c0 = to565(...hi.map(Math.round)), c1 = to565(...lo.map(Math.round));
  if (c0 < c1) [c0, c1] = [c1, c0];
  if (c0 === c1 && c0 > 0) c1 = c0 - 1;   // (four colors, never the transparent mode)
  const e0 = [((c0 >> 11) & 31) * 255 / 31, ((c0 >> 5) & 63) * 255 / 63, (c0 & 31) * 255 / 31];
  const e1 = [((c1 >> 11) & 31) * 255 / 31, ((c1 >> 5) & 63) * 255 / 63, (c1 & 31) * 255 / 31];
  const pal = [e0, e1, e0.map((v, k) => (2 * v + e1[k]) / 3), e0.map((v, k) => (v + 2 * e1[k]) / 3)];
  let bits = 0;
  px.forEach((p, i) => {
    let best = 0, bd = Infinity;
    pal.forEach((c, j) => { const d = (p[0] - c[0]) ** 2 + (p[1] - c[1]) ** 2 + (p[2] - c[2]) ** 2; if (d < bd) { bd = d; best = j; } });
    bits |= best << (2 * i);
  });
  out[o] = c0 & 255; out[o + 1] = c0 >> 8; out[o + 2] = c1 & 255; out[o + 3] = c1 >> 8;
  out[o + 4] = bits & 255; out[o + 5] = (bits >>> 8) & 255; out[o + 6] = (bits >>> 16) & 255; out[o + 7] = (bits >>> 24) & 255;
}

function encodeAlphaBlock(px, out, o) {   // DXT5 alpha: 2 endpoints, 8 levels, 3 bits a pixel
  let a0 = 0, a1 = 255;
  for (const p of px) { a0 = Math.max(a0, p[3]); a1 = Math.min(a1, p[3]); }
  if (a0 === a1) { a1 = a0 > 0 ? a0 - 1 : 0; if (a0 === 0) a0 = 1; }
  const pal = [a0, a1];
  for (let i = 1; i < 7; i++) pal.push(((7 - i) * a0 + i * a1) / 7);
  let bits = 0n;
  px.forEach((p, i) => {
    let best = 0, bd = Infinity;
    pal.forEach((c, j) => { const d = Math.abs(p[3] - c); if (d < bd) { bd = d; best = j; } });
    bits |= BigInt(best) << BigInt(3 * i);
  });
  out[o] = a0; out[o + 1] = a1;
  for (let i = 0; i < 6; i++) out[o + 2 + i] = Number((bits >> BigInt(8 * i)) & 255n);
}

function halve(rgba, w, h) {
  const nw = Math.max(1, w >> 1), nh = Math.max(1, h >> 1), out = new Uint8ClampedArray(nw * nh * 4);
  for (let y = 0; y < nh; y++) for (let x = 0; x < nw; x++) for (let k = 0; k < 4; k++) {
    let s = 0, n = 0;
    for (let dy = 0; dy < 2; dy++) for (let dx = 0; dx < 2; dx++) {
      const sx = Math.min(w - 1, x * 2 + dx), sy = Math.min(h - 1, y * 2 + dy);
      s += rgba[(sy * w + sx) * 4 + k]; n++;
    }
    out[(y * nw + x) * 4 + k] = s / n;
  }
  return {rgba: out, w: nw, h: nh};
}

// rgba (width x height) -> a DDS file: format 'DXT1' / 'DXT5' / 'DXT3' (written as DXT5) / 'RGBA', with mips levels
export function encodeDds(rgba, width, height, format, mips) {
  const dxt = format !== 'RGBA', fmt = format === 'DXT1' ? 'DXT1' : 'DXT5';
  const levels = [];
  let cur = {rgba, w: width, h: height};
  for (let l = 0; l < mips; l++) {
    const {w, h} = cur;
    if (!dxt) {
      const o = new Uint8Array(w * h * 4);
      for (let i = 0; i < w * h; i++) { o[i * 4] = cur.rgba[i * 4 + 2]; o[i * 4 + 1] = cur.rgba[i * 4 + 1]; o[i * 4 + 2] = cur.rgba[i * 4]; o[i * 4 + 3] = cur.rgba[i * 4 + 3]; }
      levels.push(o);
    } else {
      const bw = Math.max(1, (w + 3) >> 2), bh = Math.max(1, (h + 3) >> 2), bs = fmt === 'DXT1' ? 8 : 16;
      const o = new Uint8Array(bw * bh * bs);
      for (let by = 0; by < bh; by++) for (let bx = 0; bx < bw; bx++) {
        const px = [];
        for (let i = 0; i < 16; i++) {
          const x = Math.min(w - 1, bx * 4 + (i & 3)), y = Math.min(h - 1, by * 4 + (i >> 2)), j = (y * w + x) * 4;
          px.push([cur.rgba[j], cur.rgba[j + 1], cur.rgba[j + 2], cur.rgba[j + 3]]);
        }
        const at = (by * bw + bx) * bs;
        if (bs === 16) { encodeAlphaBlock(px, o, at); encodeColorBlock(px, o, at + 8, false); }
        else encodeColorBlock(px, o, at, true);
      }
      levels.push(o);
    }
    if (w === 1 && h === 1) break;
    cur = halve(cur.rgba, w, h);
  }
  const body = levels.reduce((a, b) => a + b.length, 0), out = new Uint8Array(128 + body), dv = new DataView(out.buffer);
  dv.setUint32(0, 0x20534444, true); dv.setUint32(4, 124, true);
  dv.setUint32(8, 0x1 | 0x2 | 0x4 | 0x1000 | 0x20000 | (dxt ? 0x80000 : 0x8), true);
  dv.setUint32(12, height, true); dv.setUint32(16, width, true);
  dv.setUint32(20, dxt ? levels[0].length : width * 4, true);
  dv.setUint32(28, levels.length, true);
  dv.setUint32(76, 32, true);
  if (dxt) { dv.setUint32(80, 0x4, true); for (let i = 0; i < 4; i++) out[84 + i] = fmt.charCodeAt(i); }
  else { dv.setUint32(80, 0x41, true); dv.setUint32(88, 32, true); dv.setUint32(92, 0xff0000, true); dv.setUint32(96, 0xff00, true); dv.setUint32(100, 0xff, true); dv.setUint32(104, 0xff000000, true); }
  dv.setUint32(108, 0x1000 | (levels.length > 1 ? 0x400008 : 0), true);
  let o = 128;
  for (const l of levels) { out.set(l, o); o += l.length; }
  return out;
}
