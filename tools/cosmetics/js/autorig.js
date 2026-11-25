// The auto-rigger: a humanoid model of any shape (a person from another game, a Family Guy-shaped model) on a character's
// skeleton, keeping its own shape - "auto riggers for models with no rig that would work with the game" (the user,
// 2026-11-16; "Auto, then I can adjust", 2026-11-25).
//
//  1. Its joints found from its shape (guessJoints): a model standing up, facing +z, arms out to the sides (a T- or an
//     A-pose) - the hands are the furthest out, where the arm meets the torso its shoulder, the crotch where the legs
//     part, the ankles the narrowest above the feet, the neck the narrowest above the shoulders. The same is done on
//     the character's own mesh, so both have the same landmarks the same way. The player drags any that are off.
//  2. The landmarks make SEGMENTS (pelvis to chest, shoulder to elbow...). Every bone of the character, every vertex of
//     it, is somewhere along one of its segments - so it has a place along the model's same segment (as far along, as
//     far out from it - scaled by how thick the model is there against the character). The character's mesh put there
//     is the model's shape with the character's weights on it: the model's vertices take the weights of the nearest
//     ones (models.js - as a fitted model does).
//  3. The game plays the character's animations - each bone turned from the character's rest pose, a T-pose. A model
//     standing otherwise (an A-pose: arms down at 45 degrees) is first put in the character's pose: each segment turned
//     to the character's direction, its own length kept (posed). Its bones go where the character's would on that
//     posed shape, turned as the character's are (their rest rotations the same) - so the game's rotations turn it as they
//     turn the character, and its bones are as long as its own (models.cpp: "ownShape" - each bone's own rest offset,
//     not the character's; the hips' height scaled).

import * as THREE from 'three';

// The landmarks, and the segments between them (a parent's segment before its children's)
export const LANDMARKS = ['pelvis', 'chest', 'neck', 'head', 'top',
  'shoulderL', 'elbowL', 'wristL', 'tipL', 'hipL', 'kneeL', 'ankleL', 'toeL',
  'shoulderR', 'elbowR', 'wristR', 'tipR', 'hipR', 'kneeR', 'ankleR', 'toeR'];
export const SEGMENTS = [
  ['pelvis', 'chest'], ['chest', 'neck'], ['neck', 'head'], ['head', 'top'],
  ['chest', 'shoulderL'], ['shoulderL', 'elbowL'], ['elbowL', 'wristL'], ['wristL', 'tipL'],
  ['chest', 'shoulderR'], ['shoulderR', 'elbowR'], ['elbowR', 'wristR'], ['wristR', 'tipR'],
  ['pelvis', 'hipL'], ['hipL', 'kneeL'], ['kneeL', 'ankleL'], ['ankleL', 'toeL'],
  ['pelvis', 'hipR'], ['hipR', 'kneeR'], ['kneeR', 'ankleR'], ['ankleR', 'toeR'],
];
// (which segment a segment hangs from - its parent's posing turns it too)
const SEG_PARENT = SEGMENTS.map(([a]) => SEGMENTS.findIndex(([, b]) => b === a));
// A segment's "other way" for its frame: the spine and the head face +z, an arm's frame uses up, a leg's +z
const SIDE_AXIS = SEGMENTS.map(([a, b]) => (/shoulder|elbow|wrist|tip/.test(b) ? new THREE.Vector3(0, 1, 0) : new THREE.Vector3(0, 0, 1)));

const V = (x, y, z) => new THREE.Vector3(x, y, z);

// ---- 1. the joints from the shape -------------------------------------------------------------------------------------
// Points over a mesh's surface, as many on a triangle as its share of the area (a low model's vertices are few and far
// apart - a capsule's rings miss the heights in between: a gap between the legs found in the middle of the torso).
// tris: a function (k, a, b, c) filling three THREE.Vector3s with triangle k's corners; count: how many triangles.
export function sampleSurface(count, tris, n = 40000) {
  const a = V(0, 0, 0), b = V(0, 0, 0), c = V(0, 0, 0), areas = new Float64Array(count);
  let total = 0;
  for (let k = 0; k < count; k++) {
    tris(k, a, b, c);
    total += areas[k] = b.clone().sub(a).cross(c.clone().sub(a)).length() / 2;
  }
  const out = [];
  let seed = 12345;
  const rnd = () => ((seed = (seed * 1103515245 + 12345) & 0x7fffffff) / 0x7fffffff);
  for (let k = 0; k < count; k++) {
    tris(k, a, b, c);
    out.push(a.clone(), b.clone(), c.clone());   // (the corners too: a thin piece keeps its ends)
    const m = Math.floor(areas[k] / total * n + rnd());
    for (let j = 0; j < m; j++) {
      let u = rnd(), v = rnd();
      if (u + v > 1) { u = 1 - u; v = 1 - v; }
      out.push(a.clone().addScaledVector(b.clone().sub(a), u).addScaledVector(c.clone().sub(a), v));
    }
  }
  return out;
}

// pts: THREE.Vector3s - standing on y 0, its middle at x 0, z 0, facing +z (its left at +x).
export function guessJoints(pts) {
  let H = 0, spanL = 0, spanR = 0;
  for (const p of pts) { H = Math.max(H, p.y); if (p.x > spanL) spanL = p.x; if (-p.x > spanR) spanR = -p.x; }
  const mean = list => list.reduce((a, p) => a.add(p), V(0, 0, 0)).multiplyScalar(1 / Math.max(1, list.length));
  const J = {};
  // the crotch: coming down from the middle of the body, the first height with a gap between the legs (nothing near x 0,
  // something on both sides) - from the top: thighs can touch below it (a big character's), a gap above means none
  const width = Math.max(spanL, spanR), gap = width * 0.025;
  let crotch = 0;
  for (let y = H * 0.62; y > H * 0.08; y -= H * 0.004) {
    const band = pts.filter(p => Math.abs(p.y - y) < H * 0.005);
    if (band.length < 8) continue;
    const mid = band.filter(p => Math.abs(p.x) < gap).length;
    const l = band.filter(p => p.x > gap && p.x < width * 0.5).length, r = band.filter(p => p.x < -gap && p.x > -width * 0.5).length;
    if (!mid && l >= 3 && r >= 3) { crotch = y; break; }
  }
  if (!crotch) crotch = H * 0.47;   // (a skirt, a coat to the floor: where a person's is)
  // the arms: x bins from the hand inward - an arm is thin up and down, the torso tall (only above the crotch: the legs
  // below are not the torso)
  const upper = pts.filter(p => p.y > crotch * 0.9);
  const arm = side => {
    const span = side > 0 ? spanL : spanR, bins = 40, w = span / bins;
    const binPts = [], thick = [];
    for (let k = 0; k < bins; k++) {
      const lo = span - (k + 1) * w, hi = span - k * w;
      const b = upper.filter(p => p.x * side > lo && p.x * side <= hi);
      binPts.push(b);
      let ymin = Infinity, ymax = -Infinity;
      for (const p of b) { ymin = Math.min(ymin, p.y); ymax = Math.max(ymax, p.y); }
      thick.push(b.length ? ymax - ymin : 0);
    }
    const tipPts = upper.filter(p => p.x * side > span * 0.97);
    const tip = mean(tipPts.length ? tipPts : binPts[0]);
    // the torso's edge: from the middle out, at the arms' height (from the crotch to a little above the hand - the head not in
    // it), the first bin much lower than the torso's own height there. (From the hand in, a puffy cuff or a glove's flare
    // looked like the torso starting.)
    const band = upper.filter(p => p.y < tip.y + H * 0.06 && p.y > crotch);
    const midThick = (() => {
      let ymin = Infinity, ymax = -Infinity;
      for (const p of band) if (Math.abs(p.x) < span * 0.05) { ymin = Math.min(ymin, p.y); ymax = Math.max(ymax, p.y); }
      return ymax > ymin ? ymax - ymin : H * 0.3;
    })();
    let edge = Math.floor(bins * 0.6);
    for (let k = bins - 1; k >= 0; k--) {   // (bins counted from the hand: the middle's are the last)
      const lo = span - (k + 1) * w, hi = span - k * w;
      let ymin = Infinity, ymax = -Infinity;
      for (const p of band) if (p.x * side > lo && p.x * side <= hi) { ymin = Math.min(ymin, p.y); ymax = Math.max(ymax, p.y); }
      if (!(ymax > ymin) || ymax - ymin < midThick * 0.45) { edge = k; break; }
    }
    const edgeX = span - edge * w;
    // the arm itself, followed in from the hand: in each bin only the run of heights joined to the arm's in the bin before
    // (a big head or hair hanging beside the arm, over it, is not the arm)
    const cell = H * 0.01, armPts = [];
    let cy = tip.y;
    for (let k = 0; k < bins; k++) {
      const b = binPts[k], rows = new Map();
      for (const p of b) { const c = Math.floor(p.y / cell); if (!rows.has(c)) rows.set(c, []); rows.get(c).push(p); }
      const keys = [...rows.keys()].sort((a, c) => a - c), runs = [];
      for (const c of keys) { if (runs.length && c <= runs[runs.length - 1].hi + 1) runs[runs.length - 1].hi = c; else runs.push({lo: c, hi: c}); }
      const at = Math.floor(cy / cell);
      let best = null, far = Infinity;
      for (const r of runs) { const d = at < r.lo ? r.lo - at : at > r.hi ? at - r.hi : 0; if (d < far) { far = d; best = r; } }
      if (!best || far * cell > H * 0.04) { armPts.push(armPts.length ? armPts[armPts.length - 1] : b); continue; }
      const run = [];
      for (let c = best.lo; c <= best.hi; c++) run.push(...rows.get(c));
      armPts.push(run);
      cy = mean(run).y;
    }
    const at = k => { const b = armPts[Math.max(0, Math.min(bins - 1, k))]; return b.length ? mean(b) : null; };
    // the shoulder: the arm's middle just outside the torso's edge, a little in
    const sh = at(edge - 1) || V(edgeX * side, H * 0.75, 0);
    const shoulder = V(edgeX * 0.92 * side, sh.y, sh.z);
    // the wrist: the thinnest place in the outer third of the arm, past the hand's widest
    const L = tip.distanceTo(shoulder);
    let wrist = shoulder.clone().lerp(tip, 0.8), thin = Infinity;
    for (let k = 1; k < edge * 0.4; k++) {
      const b = armPts[k];
      if (b.length < 3) continue;
      let ymin = Infinity, ymax = -Infinity, zmin = Infinity, zmax = -Infinity;
      for (const p of b) { ymin = Math.min(ymin, p.y); ymax = Math.max(ymax, p.y); zmin = Math.min(zmin, p.z); zmax = Math.max(zmax, p.z); }
      const t = (ymax - ymin) + (zmax - zmin), c = mean(b);
      const along = c.distanceTo(shoulder) / L;
      if (along < 0.65 || along > 0.93) continue;
      if (t < thin * 0.97) { thin = t; wrist = c; }
    }
    // the elbow: half way, on the arm's middle there
    const half = shoulder.clone().lerp(wrist, 0.5);
    const near = upper.filter(p => p.distanceTo(half) < L * 0.08 && p.x * side > edgeX);
    const elbow = near.length > 4 ? mean(near) : half;
    return {shoulder, elbow, wrist, tip};
  };
  for (const [s, k] of [[1, 'L'], [-1, 'R']]) {
    const a = arm(s);
    J['shoulder' + k] = a.shoulder; J['elbow' + k] = a.elbow; J['wrist' + k] = a.wrist; J['tip' + k] = a.tip;
  }
  // the legs: below the crotch, each side
  const leg = side => {
    // (the leg's column: round where it stands - an arm hanging beside it, in an A-pose, isn't the leg)
    const feet = pts.filter(p => p.y < crotch * 0.25 && p.x * side > 0);
    const fx = feet.length ? feet.reduce((a, p) => a + p.x, 0) / feet.length : side * width * 0.12;
    let reach = 0;
    for (const p of feet) reach = Math.max(reach, Math.abs(p.x - fx));
    const lp = pts.filter(p => p.y < crotch && p.x * side > 0 && Math.abs(p.x - fx) < Math.max(reach * 1.8, crotch * 0.12));
    const at = (y, r) => { const b = lp.filter(p => Math.abs(p.y - y) < r); return b.length ? mean(b) : null; };
    const top = at(crotch * 0.92, crotch * 0.05) || V(side * width * 0.15, crotch, 0);
    const hip = V(top.x, Math.min(crotch * 1.08, crotch + H * 0.04), top.z);
    // the ankle: the slimmest front to back in the leg's lowest third, above the foot
    let ankle = null, thin = Infinity;
    for (let y = crotch * 0.06; y < crotch * 0.33; y += crotch * 0.01) {
      const b = lp.filter(p => Math.abs(p.y - y) < crotch * 0.012);
      if (b.length < 4) continue;
      let zmin = Infinity, zmax = -Infinity;
      for (const p of b) { zmin = Math.min(zmin, p.z); zmax = Math.max(zmax, p.z); }
      if (zmax - zmin < thin * 0.97) { thin = zmax - zmin; ankle = mean(b); }
    }
    ankle = ankle || at(crotch * 0.15, crotch * 0.04) || V(hip.x, crotch * 0.15, hip.z);
    const half = hip.clone().lerp(ankle, 0.5);
    const knee = at(half.y, crotch * 0.03) || half;
    const low = lp.filter(p => p.y < Math.max(ankle.y * 0.6, H * 0.02));
    let zmax = -Infinity;
    for (const p of low) zmax = Math.max(zmax, p.z);
    const front = low.filter(p => p.z > zmax - (zmax - ankle.z) * 0.15);
    const toe = front.length ? mean(front) : V(ankle.x, 0, ankle.z + H * 0.08);
    return {hip, knee, ankle, toe};
  };
  for (const [s, k] of [[1, 'L'], [-1, 'R']]) {
    const g = leg(s);
    J['hip' + k] = g.hip; J['knee' + k] = g.knee; J['ankle' + k] = g.ankle; J['toe' + k] = g.toe;
  }
  // the middle: the pelvis between the hips, the chest just below the shoulders, the neck the slimmest above them
  const shY = (J.shoulderL.y + J.shoulderR.y) / 2;
  const midZ = y => { const b = pts.filter(p => Math.abs(p.y - y) < H * 0.01 && Math.abs(p.x) < width * 0.2); return b.length ? mean(b).z : 0; };
  J.pelvis = V(0, (J.hipL.y + J.hipR.y) / 2, (J.hipL.z + J.hipR.z) / 2);
  J.chest = V(0, shY - (shY - J.pelvis.y) * 0.12, midZ(shY - (shY - J.pelvis.y) * 0.12));
  let neckY = shY + (H - shY) * 0.15, narrow = Infinity;
  for (let y = shY + (H - shY) * 0.04; y < shY + (H - shY) * 0.5; y += H * 0.004) {
    const b = pts.filter(p => Math.abs(p.y - y) < H * 0.005 && Math.abs(p.x) < width * 0.35);
    if (b.length < 4) continue;
    let xmin = Infinity, xmax = -Infinity;
    for (const p of b) { xmin = Math.min(xmin, p.x); xmax = Math.max(xmax, p.x); }
    if (xmax - xmin < narrow * 0.97) { narrow = xmax - xmin; neckY = y; }
  }
  J.neck = V(0, neckY, midZ(neckY));
  const headY = neckY + (H - neckY) * 0.3;
  J.head = V(0, headY, midZ(headY));
  J.top = V(0, H, J.head.z);
  return J;
}

// The character's landmarks: from its own skeleton (rig.js names its bones) - its hips, its arms' and legs' chains - and
// its mesh for the ends (the top of its head, its finger tips, its toes' fronts). In its space, as given.
export function characterJoints(skel, pieces) {
  const pos = i => V(0, 0, 0).setFromMatrixPosition(skel.bind[i]);
  const named = re => skel.names.findIndex(n => re.test(n));
  const isMiddle = i => /^(Hips|Spine|Neck|Head|Root)/.test(skel.names[i] || '');
  const pts = [];
  for (const p of pieces) for (let i = 0; i < p.position.length; i += 3) pts.push(V(p.position[i], p.position[i + 1], p.position[i + 2]));
  const J = {};
  const hips = named(/^Hips/);
  J.pelvis = pos(hips >= 0 ? hips : 1);
  let topY = -Infinity, cx = 0;
  for (const p of pts) topY = Math.max(topY, p.y);
  let chestBone = -1;
  for (const [side, k] of [['L', 'L'], ['R', 'R']]) {
    const hand = named(new RegExp('^Hand ' + side + ' '));
    const chain = [];   // (the hand up to the middle bone it hangs from)
    for (let b = hand; b >= 0 && !isMiddle(b); b = skel.parent[b]) chain.unshift(b);
    if (chain.length && chestBone < 0) chestBone = skel.parent[chain[0]];
    const sh = chain.length >= 3 ? chain[1] : chain[0];
    const S = pos(sh), W = pos(hand), mid = S.clone().lerp(W, 0.5);
    let el = chain[Math.floor(chain.length / 2)], d = Infinity;
    for (const b of chain) { const e = pos(b).distanceTo(mid); if (b !== sh && b !== hand && e < d) { d = e; el = b; } }
    J['shoulder' + k] = S; J['elbow' + k] = pos(el); J['wrist' + k] = W;
    const out = W.clone().sub(S).normalize();   // (the furthest point of the mesh out along the arm, past the wrist)
    let best = W.clone(), far = -Infinity;
    for (const p of pts) { const t = p.clone().sub(W).dot(out); if (t > far && p.clone().sub(W).projectOnPlane(out).length() < S.distanceTo(W) * 0.5) { far = t; best = p; } }
    J['tip' + k] = W.clone().addScaledVector(out, Math.max(far, 0.01));
    const leg = [];
    for (let n = 1; ; n++) { const b = named(new RegExp('^Leg ' + side + ' ' + n + ' ')); if (b < 0) break; leg.push(b); }
    J['hip' + k] = pos(leg[0]); J['knee' + k] = pos(leg[1] ?? leg[0]); J['ankle' + k] = pos(leg[2] ?? leg[leg.length - 1]);
    const A = J['ankle' + k], low = pts.filter(p => (p.x - cx) * (side === 'L' ? 1 : -1) > 0 && p.y < A.y + 0.02);
    let zmax = -Infinity;
    for (const p of low) zmax = Math.max(zmax, p.z);
    const front = low.filter(p => p.z > zmax - 0.02);
    J['toe' + k] = front.length ? front.reduce((a, p) => a.add(p), V(0, 0, 0)).multiplyScalar(1 / front.length) : pos(leg[leg.length - 1]);
  }
  J.chest = pos(chestBone >= 0 ? chestBone : hips);
  const head = named(/^Head/), neck = named(/^Neck/);
  J.head = pos(head);
  J.neck = neck >= 0 ? pos(neck) : J.chest.clone().lerp(J.head, 0.6);
  J.top = V(J.head.x, topY, J.head.z);
  return J;
}

// ---- 2. segments: frames, a point's place along one, the same place on another ----------------------------------------
function frame(J, k) {
  const [a, b] = SEGMENTS[k], A = J[a], B = J[b];
  const u = B.clone().sub(A), len = Math.max(u.length(), 1e-6);
  u.multiplyScalar(1 / len);
  const side = SIDE_AXIS[k].clone();
  let v = side.sub(u.clone().multiplyScalar(side.dot(u)));
  if (v.lengthSq() < 1e-8) v = V(1, 0, 0).sub(u.clone().multiplyScalar(u.x));
  v.normalize();
  const w = u.clone().cross(v);
  return {A, B, u, v, w, len};
}
export function frames(J) { return SEGMENTS.map((_, k) => frame(J, k)); }
// The nearest segment to a point (a capsule's distance: along the segment, clamped), and where on it
export function nearestSegment(F, p, allowed = null) {
  let best = -1, bd = Infinity, bt = 0;
  for (let k = 0; k < F.length; k++) {
    if (allowed && !allowed(k)) continue;
    const f = F[k], d = p.clone().sub(f.A), t = Math.max(0, Math.min(1, d.dot(f.u) / f.len));
    const q = f.A.clone().addScaledVector(f.u, t * f.len), e = q.distanceToSquared(p);
    if (e < bd) { bd = e; best = k; bt = t; }
  }
  return {k: best, t: bt};
}
// a point on segment k of one figure (F, its girth G) at the same place on the other's (F2, G2)
export function carry(p, k, F, F2, G, G2) {
  const f = F[k], g = F2[k], d = p.clone().sub(f.A);
  const along = d.dot(f.u) / f.len, a = d.dot(f.v), b = d.dot(f.w);
  const s = G && G2 && G[k] > 1e-6 ? G2[k] / G[k] : g.len / f.len;
  return g.A.clone().addScaledVector(g.u, along * g.len).addScaledVector(g.v, a * s).addScaledVector(g.w, b * s);
}
// Each segment's girth: the mean distance from it of the points nearest it
export function girths(F, pts) {
  const sum = new Float64Array(F.length), n = new Float64Array(F.length);
  for (const p of pts) {
    const {k, t} = nearestSegment(F, p);
    if (k < 0 || t <= 0.05 || t >= 0.95) continue;
    const f = F[k], q = f.A.clone().addScaledVector(f.u, t * f.len);
    sum[k] += q.distanceTo(p); n[k]++;
  }
  return F.map((_, k) => (n[k] ? sum[k] / n[k] : 0));
}

// ---- 3. putting the model in the character's pose ----------------------------------------------------------------------
// The model's landmarks with each segment turned toward the character's direction, its own length kept (from the pelvis
// out). The arms all the way but a few degrees (an A-pose's raised to a T: the game's aiming is the character's); the
// legs and the rest only what's more than 15 degrees off - its own stance kept: wide-set boots stay apart (turned all the way, a chubby model's feet were put where a slim character's are, into each other). The bits
// joining the limbs on (the pelvis to a hip, the chest to a shoulder) are not turned at all: its hips level with its
// pelvis, a character's well below, turned dragged its legs up into each other.
const TURN_FREE_ARM = 3 * Math.PI / 180, TURN_FREE = 15 * Math.PI / 180;
export function posedJoints(Jm, Jc) {
  const P = {pelvis: Jm.pelvis.clone()};
  for (const [a, b] of SEGMENTS) {
    if (P[b]) continue;
    const own = Jm[b].clone().sub(Jm[a]), len = own.length();
    own.normalize();
    const to = Jc[b].clone().sub(Jc[a]).normalize(), dir = own.clone();
    const joining = (a === 'pelvis' && /^hip/.test(b)) || (a === 'chest' && /^shoulder/.test(b));
    const angle = Math.acos(Math.max(-1, Math.min(1, own.dot(to))));
    const free = /^(shoulder|elbow|wrist)/.test(a) ? TURN_FREE_ARM : TURN_FREE;
    if (!joining && angle > free) {
      const axis = own.clone().cross(to);
      if (axis.lengthSq() < 1e-12) axis.set(0, 0, 1); else axis.normalize();
      dir.applyAxisAngle(axis, angle - free);
    }
    P[b] = P[a].clone().addScaledVector(dir, len);
  }
  return P;
}
// Each segment's move from the model as it stands to the posed one: a rotation about its start, then to the posed start
export function poseMoves(Jm, P) {
  const Fm = frames(Jm), Fp = frames(P);
  return SEGMENTS.map((_, k) => {
    const m = Fm[k], p = Fp[k];
    const from = new THREE.Matrix4().makeBasis(m.u, m.v, m.w), to = new THREE.Matrix4().makeBasis(p.u, p.v, p.w);
    const R = to.multiply(from.clone().transpose());
    return {R, a: m.A, b: p.A};
  });
}
export function applyMove(mv, p) { return p.clone().sub(mv.a).applyMatrix4(mv.R).add(mv.b); }

export {SEG_PARENT};
