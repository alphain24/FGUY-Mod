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
