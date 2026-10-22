// A character's skeleton (its SkeletonBlob) and which bones move each vertex - worked out in tools/re/rig.py:
//   SkeletonBlob: +4 u16 bone count, +0x14 offset of the parent table (a byte a bone, 0xff: none), +0x18 the mirror
//   table (the bone on the other side), +0x20 the matrices: a 3x4 row-major inverse bind matrix a bone (the
//   translation in each row's 4th value).
//   A vertex moved by one bone: its last dword a u16, that bone's matrix offset (bone * 48); by more: its bytes are the
//   bones, the dword before them their weights (bytes adding up to 255).
// In the game the pose has one bone more: its bone 0 is the character's root, its bone n + 1 the skeleton's bone n.
import * as THREE from 'three';

export function parseSkeleton(d) {
  const dv = new DataView(d.buffer, d.byteOffset, d.byteLength);
  const count = dv.getUint16(4, true), parAt = dv.getUint32(0x14, true), mirAt = dv.getUint32(0x18, true);
  const matAt = dv.getUint32(0x20, true);
  const parent = [], mirror = [], invBind = [], bind = [], local = [];
  for (let i = 0; i < count; i++) {
    parent.push(d[parAt + i] === 0xff ? -1 : d[parAt + i]);
    mirror.push(d[mirAt + i] === 0xff ? -1 : d[mirAt + i]);
    const f = k => dv.getFloat32(matAt + i * 48 + k * 4, true);
    const m = new THREE.Matrix4().set(f(0), f(1), f(2), f(3), f(4), f(5), f(6), f(7), f(8), f(9), f(10), f(11), 0, 0, 0, 1);
    invBind.push(m);
    bind.push(m.clone().invert());
  }
  for (let i = 0; i < count; i++)   // each bone in its parent's space
    local.push(parent[i] < 0 ? bind[i].clone() : invBind[parent[i]].clone().multiply(bind[i]));
  return {count, parent, mirror, invBind, bind, local, names: boneNames(count, parent, mirror, bind)};
}

// Names to help find a bone: the middle of the body (no mirror) "Root", "Hips", "Spine n", the top one "Head"; a limb on
// one side (it has a mirror) "Arm L 1..", "Hand L", "Finger L 1.1.." or "Leg R..", "Foot R", "Toe R.." - the character
// faces +z, its left is +x.
function boneNames(count, parent, mirror, bind) {
  const pos = i => new THREE.Vector3().setFromMatrixPosition(bind[i]);
  const names = new Array(count).fill('');
  const centre = [...Array(count).keys()].filter(i => mirror[i] < 0 || mirror[i] === i);
  const topCentre = centre.reduce((a, b) => (pos(b).y > pos(a).y ? b : a), centre[0] ?? 0);
  let spine = 0, hips = false;
  for (const i of centre) {
    names[i] = parent[i] < 0 ? 'Root' : i === topCentre ? 'Head' : !hips ? 'Hips' : 'Spine ' + (++spine);
    if (parent[i] >= 0) hips = true;
  }
  const children = [...Array(count)].map((_, i) => [...Array(count).keys()].filter(j => parent[j] === i));
  const lowest = i => Math.min(pos(i).y, ...children[i].map(lowest));
  const widest = i => Math.max(Math.abs(pos(i).x), ...children[i].map(widest));
  const isLeg = r => pos(r).y - lowest(r) > widest(r) - Math.abs(pos(r).x);   // (a leg goes down more than out)
  // a limb (a side bone hanging off the middle): a chain ("Arm L 1, 2..."), where it branches its hand / foot, then
  // each branch a finger / toe; one hanging off the head: "Face"
  for (let r = 0; r < count; r++) {
    if (names[r] || !centre.includes(parent[r])) continue;
    const side = pos(r).x >= 0 ? 'L' : 'R';
    const kind = names[parent[r]] === 'Head' ? 'Face' : isLeg(r) ? 'Leg' : 'Arm';
    let cur = r, n = 0;
    while (children[cur].length === 1) { names[cur] = kind + ' ' + side + ' ' + ++n; cur = children[cur][0]; }
    if (!children[cur].length) { names[cur] = kind + ' ' + side + ' ' + ++n; continue; }
    names[cur] = (kind === 'Leg' ? 'Foot ' : kind === 'Arm' ? 'Hand ' : kind + ' ') + side;
    children[cur].forEach((c, b) => {
      let d = 0;
      for (let x = c; x !== undefined; x = children[x][0]) names[x] = (kind === 'Leg' ? 'Toe ' : 'Finger ') + side + ' ' + (b + 1) + '.' + ++d;
    });
  }
  for (let i = 0; i < count; i++) if (!names[i]) names[i] = 'Bone';
  return names.map((nm, i) => nm + ' (' + i + ')');   // (the number: two limbs of a kind on a side tell apart)
}

// one piece's vertex bones: for each vertex up to 4 [bone, weight] (layout: model.js vertexLayout)
export function vertexBones(vdv, layout) {
  const index = new Uint16Array(layout.length * 4), weight = new Float32Array(layout.length * 4);
  layout.forEach(({o, base, k}, v) => {
    const end = o + base + (k > 1 ? 4 : 0);
    if (k === 1) {
      index[v * 4] = vdv.getUint16(end - 4, true) / 48;
      weight[v * 4] = 1;
      return;
    }
    let total = 0;
    for (let j = 0; j < Math.min(k, 4); j++) {
      index[v * 4 + j] = vdv.getUint8(end - 4 + j);
      weight[v * 4 + j] = vdv.getUint8(end - 8 + j);
      total += weight[v * 4 + j];
    }
    for (let j = 0; j < 4; j++) weight[v * 4 + j] /= total || 1;
  });
  return {index, weight};
}
