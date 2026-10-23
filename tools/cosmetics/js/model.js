// A character model (a Model asset) -> its pieces: positions, normals, texture coordinates, triangles, and the color
// texture each piece's material uses. Layout worked out in tools/re/ho2glb.py (research):
//   Model -> SkinGeometry pieces; a piece's record: +0x54 its vertex count, ids of its Material and of three blobs - the
//   vertices (the biggest), the texture coordinates (half floats, 4 bytes a vertex), the triangles (16-bit indices).
//   A vertex: position, normal (12 + 12), with tangents also tangent + sign (16), then - moved by more than one bone -
//   its weights (4 bytes), then its bones (4). The record has a table of vertex counts by bone count (one-bone ones first).
import {T} from './ho.js';
import {parseSkeleton, vertexBones} from './rig.js';

const u32 = (dv, o) => dv.getUint32(o, true);

function boneTables(d, n) {
  const dv = new DataView(d.buffer, d.byteOffset, d.byteLength), out = [];
  for (let i = 0x80; i + 4 <= d.length; i += 4) {
    const tab = [];
    let j = i, last = 0;
    while (j + 4 <= d.length) {
      const v = u32(dv, j), k = v >>> 16, c = v & 0xffff;
      if (!(last < k && k <= 8) || !(c > 0 && c <= n) || (!tab.length && k !== 1 && k > 4)) break;
      for (let z = last + 1; z < k; z++) tab.push(0);
      tab.push(c);
      last = k; j += 4;
    }
    if (tab.length && tab.reduce((a, b) => a + b, 0) === n) out.push(tab);
  }
  return out.length ? out : [[n]];
}

// each vertex: where it starts (o), its size without the weights (base), how many bones move it (k)
function vertexLayout(d, n, vbSize) {
  for (const tab of boneTables(d, n)) for (const tangents of [false, true]) {
    const base = 28 + (tangents ? 16 : 0);
    if (tab[0] * base + (n - tab[0]) * (base + 4) !== vbSize) continue;
    const out = [];
    let o = 0;
    tab.forEach((cnt, k) => { for (let i = 0; i < cnt; i++) { out.push({o, base, k: k + 1}); o += base + (k > 0 ? 4 : 0); } });
    return out;
  }
  if (vbSize % n === 0 && vbSize / n >= 24) return Array.from({length: n}, (_, i) => ({o: i * (vbSize / n), base: vbSize / n, k: 0}));
  return null;
}

const half = new DataView(new ArrayBuffer(4));
function halfToFloat(h) {
  const s = h >> 15, e = (h >> 10) & 31, f = h & 1023;
  if (e === 0) return (s ? -1 : 1) * Math.pow(2, -14) * (f / 1024);
  if (e === 31) return f ? NaN : (s ? -Infinity : Infinity);
  return (s ? -1 : 1) * Math.pow(2, e - 15) * (1 + f / 1024);
}

// the material's color texture: a Texture asset (16 bytes: the id of its DDS blob)
async function materialTexture(ho, mat) {
  for (const {asset} of ho.refs(await mat.bytes())) {
    if (asset.type !== T.Texture) continue;
    const b = await asset.bytes(), dv = new DataView(b.buffer, b.byteOffset, b.byteLength);
    const blob = ho.get(u32(dv, 0), u32(dv, 4));
    if (blob) return {name: asset.name, texture: asset, blob};
  }
  return null;
}

// An outfit's picture in the game's skin pickers (Texture "Costume_<Character><Outfit>.tif" - Adam West is "West"),
// as materialTexture gives a texture - or null
export async function pictureFor(ho, character, outfit) {
  const name = 'Costume_' + (character === 'AdamWest' ? 'West' : character) + outfit + '.tif';
  const asset = ho.assets.find(a => a.type === T.Texture && a.name === name);
  if (!asset) return null;
  const b = await asset.bytes(), dv = new DataView(b.buffer, b.byteOffset, b.byteLength);
  const blob = ho.get(u32(dv, 0), u32(dv, 4));
  return blob ? {name: asset.name, texture: asset, blob} : null;
}

export async function loadModel(ho, model) {
  const pieces = [];
  for (const {asset: g} of ho.refs(await model.bytes())) {
    if (g.type !== T.SkinGeometry) continue;
    const d = await g.bytes(), dv = new DataView(d.buffer, d.byteOffset, d.byteLength);
    const n = dv.getUint16(0x54, true);
    const refs = ho.refs(d);
    const raws = refs.map(r => r.asset).filter(a => a.type === T.RawBlob);
    const mat = refs.map(r => r.asset).find(a => a.type === T.Material);
    const uv = raws.find(a => a.size === n * 4);
    const rest = raws.filter(a => a !== uv).sort((a, b) => b.size - a.size);
    const vb = rest[0], ib = rest[1];
    if (!vb || !ib || !n) continue;
    const layout = vertexLayout(d, n, vb.size);
    if (!layout) { console.warn('piece not decoded', g.name, n, vb.size); continue; }
    const vbytes = await vb.bytes(), vdv = new DataView(vbytes.buffer, vbytes.byteOffset, vbytes.byteLength);
    const position = new Float32Array(n * 3), normal = new Float32Array(n * 3), uvs = new Float32Array(n * 2);
    layout.forEach(({o}, i) => {
      for (let k = 0; k < 3; k++) {
        position[i * 3 + k] = vdv.getFloat32(o + 4 * k, true);
        normal[i * 3 + k] = vdv.getFloat32(o + 12 + 4 * k, true);
      }
    });
    if (uv) {
      const ub = await uv.bytes(), udv = new DataView(ub.buffer, ub.byteOffset, ub.byteLength);
      for (let i = 0; i < n * 2; i++) uvs[i] = halfToFloat(udv.getUint16(i * 2, true));
    }
    const ibytes = await ib.bytes();
    let index = new Uint16Array(ibytes.buffer.slice(ibytes.byteOffset, ibytes.byteOffset + (ibytes.byteLength & ~1)));
    index = index.subarray(0, index.length - index.length % 3);
    if (index.some(i => i >= n)) { console.warn('piece triangles out of range', g.name); continue; }
    const skin = layout[0].k ? vertexBones(vdv, layout) : null;   // (k 0: a layout not understood - no bones)
    pieces.push({position, normal, uv: uvs, index, skin, material: mat, texture: mat ? await materialTexture(ho, mat) : null});
  }
  return pieces;
}

// the model's skeleton (its SkeletonBlob)
export async function loadSkeleton(ho, model) {
  const blob = ho.refs(await model.bytes()).map(r => r.asset).find(a => a.type === T.SkeletonBlob);
  return blob ? parseSkeleton(await blob.bytes()) : null;
}

// The playable characters and their outfits (Model assets "<Character><Outfit>_Rig_Geo")
export const CHARACTERS = ['Peter', 'Stewie', 'Brian', 'Lois', 'Meg', 'Chris', 'Quagmire', 'Cleveland', 'AdamWest', 'Death'];
export function outfitsOf(ho) {
  const seen = new Map();
  for (const a of ho.assets) {
    if (a.type !== T.Model || !a.name.endsWith('_Rig_Geo') || seen.has(a.name)) continue;
    const ch = CHARACTERS.find(c => a.name.startsWith(c) && !a.name.startsWith('SpaceMask'));
    if (!ch) continue;
    const outfit = a.name.slice(ch.length, -'_Rig_Geo'.length) || 'Default';
    seen.set(a.name, {character: ch, outfit, model: a});
  }
  return [...seen.values()];
}
