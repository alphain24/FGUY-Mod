// Motion capture: a video of one person becomes an emote on the character in the Emotes tab.
// The person is tracked in the browser (MediaPipe's pose landmarker: 33 points of the body, in metres about the hips -
// nothing is uploaded; its code and model come from their public hosts the first time). The points say which way each
// part of the body lies; the character's own bones are turned to lie the same way - its hips, chest and head by the
// frame the points make (hips across, shoulders across, ears across with the nose ahead), each arm and leg bone from
// its joint to the next. Lengths don't matter: a short-armed character does the dance with its own arms.
// The landmarker's space: x to the image's right, y down, z away from the camera. A person facing the camera has their
// left at the image's right; the character faces +z with its left at +x and y up - so a point is (x, -y, -z).
import * as THREE from 'three';

const VISION = 'https://cdn.jsdelivr.net/npm/@mediapipe/tasks-vision@0.10.14';
const MODEL = 'https://storage.googleapis.com/mediapipe-models/pose_landmarker/pose_landmarker_heavy/float16/1/pose_landmarker_heavy.task';
// (the hands and the face: MediaPipe's holistic landmarker - 21 points a hand, 478 on the face - beside the pose's 33)
const HOLISTIC = 'https://storage.googleapis.com/mediapipe-models/holistic_landmarker/holistic_landmarker/float16/latest/holistic_landmarker.task';
const L = {nose: 0, earL: 7, earR: 8, shL: 11, shR: 12, elL: 13, elR: 14, wrL: 15, wrR: 16, pinkyL: 17, pinkyR: 18, indexL: 19,
  indexR: 20, hipL: 23, hipR: 24, kneeL: 25, kneeR: 26, ankL: 27, ankR: 28, heelL: 29, heelR: 30, toeL: 31, toeR: 32};
// A frame's points past the body's 33 (v 0 where the tracker had none): each hand's 21 - wrist, then thumb, first,
// middle, ring and little finger, four joints each from the hand outward - in the body's axes and put so the hand's
// wrist is the body's; then the head's own three directions (across to its left, up, ahead), each one long, from the
// face's points.
const HAND_L = 33, HAND_R = 54, HEAD = 75, POINTS = 78;
const FINGER = {thumb: 1, index: 5, middle: 9, ring: 13, pinky: 17};
// the lines drawn over the video
const LINES = [[11, 12], [11, 13], [13, 15], [12, 14], [14, 16], [11, 23], [12, 24], [23, 24], [23, 25], [25, 27], [27, 31],
  [24, 26], [26, 28], [28, 32], [0, 7], [0, 8]];

// Which of a skeleton's bones are the body's parts - from its shape (the bones' names aren't reliable on every character).
// Hands: a pair of side bones with three or more fingers on each. An arm: from the hand up to the first bone in the
// middle of the body (the chest); its shoulder the second bone when there's a collarbone before it, its elbow the bone
// nearest half way along. Legs: the pair hanging from a middle bone (the hips) that goes down; hip, knee, ankle, toe.
// The head: from the chest's middle bone up, while one bone follows another.
export function mapBody(skel) {
  const n = skel.count, parent = skel.parent, mirror = skel.mirror;
  const at = i => new THREE.Vector3().setFromMatrixPosition(skel.bind[i]);
  const kids = [...Array(n)].map((_, i) => [...Array(n).keys()].filter(j => parent[j] === i));
  const centre = i => mirror[i] < 0 || mirror[i] === i;
  const size = i => kids[i].reduce((s, c) => s + 1 + size(c), 0);
  const hands = [...Array(n).keys()].filter(i => !centre(i) && kids[i].length >= 3 && kids[i].every(c => size(c) <= 3));
  let handL = -1, handR = -1;
  for (const h of hands) {
    const m = mirror[h];
    if (m < 0 || !hands.includes(m)) continue;
    if (at(h).x >= at(m).x) { handL = h; handR = m; } else { handL = m; handR = h; }
    break;
  }
  if (handL < 0) return null;
  const arm = hand => {
    const chain = [];
    for (let b = hand; b >= 0 && !centre(b); b = parent[b]) chain.unshift(b);
    if (chain.length < 3) return null;
    const first = chain.length >= 4 ? 1 : 0, sh = chain[first];
    let total = 0;
    const along = [0];
    for (let i = first + 1; i < chain.length; i++) { total += at(chain[i]).distanceTo(at(chain[i - 1])); along.push(total); }
    let el = first + 1, best = Infinity;
    for (let i = first + 1; i < chain.length - 1; i++) {
      const d = Math.abs(along[i - first] - total / 2);
      if (d < best) { best = d; el = i; }
    }
    const end = f => { let b = f; while (kids[b].length) b = kids[b][0]; return b; };   // (a finger's last joint)
    // the fingers that bend (a bone on the hand with nothing on it is a place to hold things, not a finger). The thumb:
    // the one that starts nearest the wrist; the first finger the one beside it, the little finger the one furthest.
    const real = kids[hand].filter(f => kids[f].length);
    let index = -1, pinky = -1;
    if (real.length >= 3) {
      const thumb = real.reduce((a, b) => (at(b).distanceTo(at(hand)) < at(a).distanceTo(at(hand)) ? b : a));
      const rest = real.filter(f => f !== thumb).sort((a, b) => at(a).distanceTo(at(thumb)) - at(b).distanceTo(at(thumb)));
      index = rest[0]; pinky = rest[rest.length - 1];
    }
    // (each finger's bones from the hand outward, and which of a person's fingers it is)
    const run = f => { const out = [f]; while (kids[out[out.length - 1]].length) out.push(kids[out[out.length - 1]][0]); return out; };
    const chains = [];
    if (index >= 0) {
      const thumb = real.find(f => f !== index && f !== pinky && at(f).distanceTo(at(hand)) <= Math.min(...real.map(r => at(r).distanceTo(at(hand)))) + 1e-9);
      const others = real.filter(f => f !== thumb).sort((a, b) => at(a).distanceTo(at(thumb ?? index)) - at(b).distanceTo(at(thumb ?? index)));
      const names = others.length >= 4 ? ['index', 'middle', 'ring', 'pinky'] : others.length === 3 ? ['index', 'middle', 'pinky'] : others.length === 2 ? ['index', 'pinky'] : ['middle'];
      if (thumb !== undefined) chains.push({finger: 'thumb', bones: run(thumb)});
      others.slice(0, names.length).forEach((f, i) => chains.push({finger: names[i], bones: run(f)}));
    }
    return {clav: first ? chain[0] : -1, shoulder: sh, elbow: chain[el], hand, chest: parent[chain[0]], fingers: kids[hand],
      tips: real.map(end), index, pinky, chains, below: chain.slice(first + 1)};   // (below: the arm's bones after the shoulder, to the hand)
  };
  const armL = arm(handL), armR = arm(handR);
  if (!armL || !armR || armL.chest !== armR.chest || armL.chest < 0) return null;
  const chest = armL.chest;
  // legs: side bones on a middle bone, not the arms', that go down more than out
  const lowest = i => Math.min(at(i).y, ...kids[i].map(lowest));
  const roots = [...Array(n).keys()].filter(i => !centre(i) && parent[i] >= 0 && centre(parent[i]) && i !== (armL.clav >= 0 ? armL.clav : armL.shoulder) &&
    i !== (armR.clav >= 0 ? armR.clav : armR.shoulder) && mirror[i] >= 0 && at(i).y - lowest(i) > at(i).y * 0.5);
  let legRootL = -1, legRootR = -1;
  for (const r of roots) {
    const m = mirror[r];
    if (!roots.includes(m)) continue;
    if (at(r).x >= at(m).x) { legRootL = r; legRootR = m; } else { legRootL = m; legRootR = r; }
    break;
  }
  const leg = root => {
    if (root < 0) return null;
    const chain = [root];
    while (kids[chain[chain.length - 1]].length) {   // (down the longest way)
      const k = kids[chain[chain.length - 1]];
      chain.push(k.reduce((a, b) => (size(b) > size(a) ? b : a), k[0]));
    }
    if (chain.length < 3) return null;
    // the ankle: the last bone before the foot turns forward - the lowest joint but the last, or the last of three
    const ankle = chain.length >= 4 ? chain.length - 2 : chain.length - 1;
    let total = 0;
    const along = [0];
    for (let i = 1; i <= ankle; i++) { total += at(chain[i]).distanceTo(at(chain[i - 1])); along.push(total); }
    let knee = 1, best = Infinity;
    for (let i = 1; i < ankle; i++) {
      const d = Math.abs(along[i] - total / 2);
      if (d < best) { best = d; knee = i; }
    }
    return {hip: chain[0], knee: chain[knee], ankle: chain[ankle], toe: ankle + 1 < chain.length ? chain[ankle + 1] : -1};
  };
  // (no legs - Death's robe: the body's turned from the hips all the same; the hips then the first middle bone on the root)
  const legL = leg(legRootL), legR = leg(legRootR), legs = legL && legR;
  let hips = legs ? parent[legRootL] : chest;
  if (!legs) while (parent[hips] >= 0 && parent[parent[hips]] >= 0) hips = parent[hips];
  let head = kids[chest].find(c => centre(c));
  if (head === undefined) return null;
  for (;;) {
    const up = kids[head].filter(c => centre(c) && at(c).y > at(head).y);
    if (kids[head].length !== 1 || !up.length) break;
    head = up[0];
  }
  return {hips, chest, head, armL, armR, legL: legs ? legL : null, legR: legs ? legR : null};
}

// The character's bones for one frame of points (each {x, y, z, v} in the character's axes; v how sure). `rest`: the
// bones' own turns and places ({q, p}, in the parent's space); `prev`: the pose before this one - a part whose points
// aren't sure keeps its turn from there.
export function retarget(skel, body, rest, P, prev, opt = {}) {
  const n = skel.count, parent = skel.parent;
  const q = rest.map(r => new THREE.Quaternion().fromArray(r.q)), p = rest.map(r => new THREE.Vector3().fromArray(r.p));
  const mq = new Array(n), mp = new Array(n);   // (in the character's space)
  const fk = () => {
    for (let i = 0; i < n; i++) {
      if (parent[i] < 0) { mq[i] = q[i].clone(); mp[i] = p[i].clone(); continue; }
      mq[i] = mq[parent[i]].clone().multiply(q[i]);
      mp[i] = p[i].clone().applyQuaternion(mq[parent[i]]).add(mp[parent[i]]);
    }
  };
  // a bone turned in the character's space by `d` (what hangs on it comes along)
  const turn = (b, d) => {
    const want = d.clone().multiply(mq[b]);
    q[b] = parent[b] < 0 ? want : mq[parent[b]].clone().invert().multiply(want);
    fk();
  };
  // (how sure the tracker is of a point: under 0.3 it's a guess. It was 0.45 at first: on the test clip the arm in front
  // of the face is 0.38 to 0.45 for a second and a half, and one knee for 53 frames of 181 - both stood still)
  const sure = (...ids) => Math.min(...ids.map(i => P[i].v)) >= (opt.sure ?? 0.3);
  const keep = b => { if (prev) q[b].fromArray(prev[b].q); fk(); };
  const V = i => new THREE.Vector3(P[i].x, P[i].y, P[i].z);
  const mid = (a, b) => a.clone().add(b).multiplyScalar(0.5);
  // a frame from what lies across (to the left) and a hint of up: x left, y up, z ahead
  const frame = (across, up) => {
    const x = across.clone().normalize(), y = up.clone().addScaledVector(x, -up.dot(x)).normalize(), z = x.clone().cross(y);
    return new THREE.Quaternion().setFromRotationMatrix(new THREE.Matrix4().makeBasis(x, y, z));
  };
  fk();
  const A = body.armL, B = body.armR, C = body.legL, D = body.legR;
  // (at rest: where the feet stand, how high the hips are)
  const stood = C ? [mp[C.ankle].clone(), mp[D.ankle].clone()] : null, tall = mp[body.hips].y - (opt.floor ?? 0);
  // hips: the hips across, the shoulders above them
  const upT = mid(V(L.shL), V(L.shR)).sub(mid(V(L.hipL), V(L.hipR)));
  const upNow = () => mid(mp[A.shoulder], mp[B.shoulder]).sub(C ? mid(mp[C.hip], mp[D.hip]) : mp[body.hips]);
  if (sure(L.hipL, L.hipR, L.shL, L.shR)) {
    const now = frame(C ? mp[C.hip].clone().sub(mp[D.hip]) : mp[A.shoulder].clone().sub(mp[B.shoulder]), upNow()), want = frame(V(L.hipL).sub(V(L.hipR)), upT);
    turn(body.hips, want.multiply(now.invert()));
    // chest: the shoulders across
    const now2 = frame(mp[A.shoulder].clone().sub(mp[B.shoulder]), upNow()), want2 = frame(V(L.shL).sub(V(L.shR)), upT);
    turn(body.chest, want2.multiply(now2.invert()));
  } else { keep(body.hips); keep(body.chest); }
  // head: its turn against the chest (headTurn) - from the face's own points as it is; from the ears and nose (no face
  // in this frame) less what those read on a level head (opt.headZero - headLevel) and the tilt at 0.6 (opt.roll: it's
  // from the ears' heights alone, and an ear is often behind a hand). No more than a neck allows.
  let headM = null;   // (the dancer's head: its frame, in the character's axes)
  if (sure(L.earL, L.earR, L.nose)) {
    const {chest: chestM, turn: e, face} = headTurn(P, opt.face !== false), lim = (v, a) => Math.max(-a, Math.min(a, v));
    const zero = face ? {nod: 0, tilt: 0} : opt.headZero || {nod: 0, tilt: 0};
    e.set(lim(e.x - zero.nod, 0.6), lim(e.y, 1.2), lim((e.z - zero.tilt) * (face ? 1 : opt.roll ?? 0.6), 0.35), 'YXZ');
    const want = chestM.clone().multiply(new THREE.Quaternion().setFromEuler(e));
    headM = want.clone();
    const restM = restModel(skel, rest, body.head), full = want.multiply(restM);
    const now = mq[body.head].clone();
    now.slerp(full, opt.head ?? 0.8);
    q[body.head] = mq[parent[body.head]].clone().invert().multiply(now);
    fk();
  } else keep(body.head);
  // a limb's bone: from its joint to the next, the way the points lie
  const aim = (bone, next, a, b) => {
    if (bone < 0 || next < 0) return;
    if (!sure(a, b)) { keep(bone); return; }
    const now = mp[next].clone().sub(mp[bone]), want = V(b).sub(V(a));
    if (now.lengthSq() < 1e-10 || want.lengthSq() < 1e-10) return;
    turn(bone, new THREE.Quaternion().setFromUnitVectors(now.normalize(), want.normalize()));
  };
  // (a hand's points are less sure than the arm's - it's small, and over a face half hidden: taken at less)
  const handSure = (...ids) => Math.min(...ids.map(i => P[i].v)) >= (opt.handSure ?? 0.2);
  // A hand's points for its arm: the wrist (W), the first and little fingers' knuckles (I, K) and the middle of the
  // fingertips (F). From the hand's own 21 points where the tracker had the hand (.full); else from the body's three - a
  // wrist and two knuckles, the fingertips taken as far past the knuckles as those are past the wrist.
  const handOf = (base, wr, index, pinky) => {
    const W = V(wr);
    if (opt.hands !== false && P.length > base + 20 && P[base].v > 0) {
      const h = i => V(base + i);
      return {W, I: h(FINGER.index), K: h(FINGER.pinky), F: h(8).add(h(12)).add(h(16)).add(h(20)).multiplyScalar(0.25), ok: handSure(wr), full: true, base};
    }
    const I = V(index), K = V(pinky), M = mid(I, K);
    return {W, I, K, F: M.clone().add(M).sub(W), ok: handSure(wr, index, pinky), full: false, base};
  };
  const knucklesOf = arm => arm.fingers.reduce((s2, f) => s2.add(mp[f]), new THREE.Vector3()).multiplyScalar(1 / arm.fingers.length);
  // The hand: the way it lies (wrist to knuckles), then which way its palm faces - the first finger's knuckle to the little
  // finger's, turned about the way it lies till the character's own two lie the same. Without it a hand over the eyes
  // was edge-on to the face. Most of that turn is the forearm's (it's the forearm that turns a palm over, and a wrist
  // turned half round on its own is wrung like a cloth): opt.forearm of it there, the rest at the wrist.
  const lieOver = new Map();   // (an arm's hand turned another way than the dancer's, and how much: reach, when the arm's too short)
  const grown = new Map();     // (an arm made longer, and by how much: reach)
  const lieTo = (arm, g) => {
    const now = knucklesOf(arm).sub(mp[arm.hand]), want = mid(g.I, g.K).sub(g.W);
    if (now.lengthSq() < 1e-10 || want.lengthSq() < 1e-10) return null;
    want.normalize();
    const o = lieOver.get(arm), dir = o && o.to.clone().sub(mp[arm.hand]);   // (from where the wrist is now to the fingertips' place)
    if (dir && dir.lengthSq() > 1e-10) { dir.normalize(); want.lerp(dir, o.w); if (want.lengthSq() < 1e-10) want.copy(dir); want.normalize(); }
    turn(arm.hand, new THREE.Quaternion().setFromUnitVectors(now.normalize(), want));
    return want;
  };
  const palmOff = (arm, lie, g) => {   // (how far round the palm is from the dancer's, about the way the hand lies)
    const a = mp[arm.index].clone().sub(mp[arm.pinky]), b = g.I.clone().sub(g.K);
    a.addScaledVector(lie, -a.dot(lie)); b.addScaledVector(lie, -b.dot(lie));
    if (a.lengthSq() < 1e-10 || b.length() < (opt.palmMin ?? 0.012)) return 0;
    a.normalize(); b.normalize();
    return Math.atan2(a.clone().cross(b).dot(lie), a.dot(b));
  };
  const handAim = (arm, g, palm) => {
    if (!arm.fingers.length) return;
    if (!g.ok) { keep(arm.hand); return; }
    let lie = lieTo(arm, g);
    if (!palm || !lie || arm.index < 0 || opt.palm === false) return;
    const off = palmOff(arm, lie, g), fore = mp[arm.hand].clone().sub(mp[arm.elbow]).normalize();
    const share = (opt.forearm ?? 0.65) * Math.max(0, fore.dot(lie));   // (a hand bent square to the forearm isn't turned by it)
    if (share * Math.abs(off) > 1e-4) {
      turn(arm.elbow, new THREE.Quaternion().setFromAxisAngle(fore, off * share));
      lie = lieTo(arm, g);
      if (!lie) return;
    }
    turn(arm.hand, new THREE.Quaternion().setFromAxisAngle(lie, palmOff(arm, lie, g)));
  };
  // the arm bent so its wrist is at T: shoulder and elbow turned, the elbow kept to the side it's on
  const wristTo = (arm, T) => {
    const S = mp[arm.shoulder].clone(), El = mp[arm.elbow].clone(), l1 = El.distanceTo(S), l2 = mp[arm.hand].distanceTo(El);
    const dir = T.clone().sub(S);
    let dist = dir.length();
    if (dist < 1e-6 || l1 < 1e-6 || l2 < 1e-6) return;
    const u = dir.clone().multiplyScalar(1 / dist);
    dist = Math.max(Math.abs(l1 - l2) + 1e-3, Math.min(l1 + l2 - 1e-3, dist));
    const ca = Math.max(-1, Math.min(1, (l1 * l1 + dist * dist - l2 * l2) / (2 * l1 * dist))), sa = Math.sqrt(1 - ca * ca);
    const side = El.clone().sub(S);
    side.addScaledVector(u, -side.dot(u));
    if (side.lengthSq() < 1e-10) side.set(0, -1, 0).addScaledVector(u, u.y);
    side.normalize();
    const E2 = S.clone().addScaledVector(u, l1 * ca).addScaledVector(side, l1 * sa), T2 = S.clone().addScaledVector(u, dist);
    turn(arm.shoulder, new THREE.Quaternion().setFromUnitVectors(El.sub(S).normalize(), E2.clone().sub(S).normalize()));
    const now = mp[arm.hand].clone().sub(mp[arm.elbow]), want = T2.sub(mp[arm.elbow]);
    if (now.lengthSq() > 1e-10 && want.lengthSq() > 1e-10) turn(arm.elbow, new THREE.Quaternion().setFromUnitVectors(now.normalize(), want.normalize()));
  };
  // A hand at the face: where the dancer's fingertips are against their own eyes, the character's go against its own - a
  // hand over the eyes, a finger at the temple. Turned the dancer's way bone by bone, a character with a big head and
  // short arms (all of these) had the hand at its chin; put by the wrist or the knuckles it sat beside the hair, the
  // fingers poking into the head (the dancer's knuckles are at his temple - it's his fingers that lie over his eyes).
  // The fingertips (a finger's length past the knuckles, the way the hand lies) from between the dancer's eyes, in the
  // head's own frame and in head-widths, are put as far from between the character's eyes (opt.shape.face - its place
  // on the head bone - and .head: half the head's width); the wrist a hand's length short of that the way the hand is
  // to lie; the arm bends to it. All of it within `near` head-widths of the eyes, none beyond `far`: a hand held out is
  // the arm's own turn.
  const tipsOf = arm => {   // (the end of the hand: the middle of the fingers' last joints)
    const ends = arm.tips.length ? arm.tips : arm.fingers;
    return ends.reduce((s2, f) => s2.add(mp[f]), new THREE.Vector3()).multiplyScalar(1 / ends.length);
  };
  const reach = (arm, g) => {
    const sh = opt.shape;
    if (!sh || !headM || !arm.fingers.length || !sure(L.earL, L.earR) || !g.ok) return;
    const rm = V(L.earL).distanceTo(V(L.earR)) * 0.575;
    if (rm < 1e-4) return;
    const lie = mid(g.I, g.K).sub(g.W);
    if (lie.lengthSq() < 1e-10) return;
    lie.normalize();
    const o = g.F.clone().sub(mid(V(2), V(5))), d = o.length() / rm, near = opt.near ?? 2.2, far = opt.far ?? 4.2;
    let w = (far - d) / (far - near);
    if (w <= 0) return;
    w = w > 1 ? 1 : w * w * (3 - 2 * w);
    const head = body.head, turned = mq[head].clone().multiply(restModel(skel, rest, head).invert());   // (the character's head: from rest to now)
    const eyes = sh.face.clone().applyQuaternion(mq[head]).add(mp[head]);
    const oh = o.applyQuaternion(headM.clone().invert()).multiplyScalar(1 / rm);   // (in the head's frame, in half head-widths)
    // (never behind the face: the tracker has a hand that's over the eyes anywhere from half a head-width inside the
    // head to as far in front of it - what's over the face is put a hand's thickness in front of it)
    const over = 1 - (oh.x * oh.x + oh.y * oh.y) / 1.44;
    if (over > 0) oh.z = Math.max(oh.z, (opt.faceGap ?? 0.1) * Math.sqrt(over));   // (clear puts it on the face's own surface)
    // Across and ahead by the head's width; up and down by how far the character's own eyes are from the top of its
    // head and from its chin (a person's: 1.45 half-widths each way - Lois's eyes are near the top of her head, and
    // fingertips at the dancer's brow were put in her hair). That for as far as the head's own size goes; what's beyond
    // by the arms' lengths - two head-widths out from a head twice as big against its body was the arm at full stretch.
    const dh = oh.length(), armsT = V(L.shL).distanceTo(V(L.elL)) + V(L.elL).distanceTo(V(L.wrL));
    const S = mp[arm.shoulder], l1 = mp[arm.elbow].distanceTo(S), l2 = mp[arm.hand].distanceTo(mp[arm.elbow]);
    oh.set(oh.x * sh.head.r.x, oh.y * (oh.y > 0 ? sh.up : sh.down) / 1.45, oh.z * sh.head.r.x);
    const mh = oh.length();
    if (dh > 1 && armsT > 1e-4 && mh > 1e-6) oh.multiplyScalar((mh / dh + (dh - 1) * rm * (l1 + l2) / armsT) / mh);
    const Tt = oh.applyQuaternion(turned).add(eyes);   // (the fingertips' place)
    const long = tipsOf(arm).distanceTo(mp[arm.hand]), longer = grown.get(arm) || 1, R0 = (l1 + l2) * 0.985 / longer;
    let T = Tt.clone().addScaledVector(lie, -long);   // (the wrist, for the fingertips there with the hand lying the dancer's way)
    lieOver.delete(arm);
    // Out of the arm's reach - Lois can't lay a flat hand over her own eyes: her wrist would have to be 0.47 from her
    // shoulder, and her arm is 0.41 long. The arm is made longer for it, up to 1.3 times (opt.stretch: its bones after
    // the shoulder set further along - a cartoon's arm, and it's against the face, where the eye is on the hand).
    // (only for a hand at the face itself - within 1.2 half head-widths of the eyes, none past 2: a hand held up beside
    // the head was a long straight arm)
    const touch = Math.max(0, Math.min(1, (2 - d) / 0.8));
    const need = T.distanceTo(S), f = need > R0 ? 1 + (Math.min(opt.stretch ?? 1.3, need / R0) - 1) * w * touch * touch * (3 - 2 * touch) : 1;
    if (Math.abs(f - longer) > 1e-4) {
      for (const b of arm.below) p[b].fromArray(rest[b].p).multiplyScalar(f);
      fk();
      grown.set(arm, f);
    }
    const R = R0 * f;
    // Still out of reach: the fingertips' place comes first - the wrist goes where the arm can put it a hand's length
    // from there, as near as that gets to where it was wanted, and the hand points from it to the place.
    if (T.distanceTo(S) > R) {
      const ax = Tt.clone().sub(S), D = ax.length();
      ax.multiplyScalar(1 / D);
      if (D >= R + long) T = S.clone().addScaledVector(ax, R);
      else {
        const a = (R * R - long * long + D * D) / (2 * D), h = Math.sqrt(Math.max(0, R * R - a * a)), c = S.clone().addScaledVector(ax, a);
        const side = T.clone().sub(c);
        side.addScaledVector(ax, -side.dot(ax));
        if (side.lengthSq() < 1e-10) side.set(0, -1, 0).addScaledVector(ax, ax.y);
        T = c.addScaledVector(side.normalize(), h);
      }
      lieOver.set(arm, {to: Tt, w});
    }
    T.lerpVectors(mp[arm.hand], T, w);
    wristTo(arm, T);
  };
  // Nothing of a hand inside the head or the body (opt.shape.head / .body, each on its bone): the wrist, the knuckles,
  // the fingers' middles and their tips are tried; the one deepest in says how far the wrist has to go, and the arm
  // bends again to that. Three times: one push can turn the hand into the other. In front (ahead of the part's middle,
  // within its width and height): the part's own front, as its vertices have it (.front - frontOf), and a hand's
  // thickness; the hand goes straight ahead to that - a hand comes off a face or a chest forwards. An egg about the
  // part stood in for its front at first: Lois's is as deep as her nose is long, and a hand over her eyes hung a hand's
  // length in front of them; pushed out from the egg's middle, it slid round to her temple. Behind the middle: the egg
  // (.r), straight out from its middle.
  const clear = arm => {
    const sh = opt.shape;
    if (!sh || opt.clip === false) return;
    const thick = (opt.thick ?? 0.09) * sh.head.r.x;
    for (let pass = 0; pass < 3; pass++) {
      let most = 0, push = null;
      const knuckles = knucklesOf(arm), tips = tipsOf(arm);
      for (const [egg, bone] of [[sh.head, body.head], [sh.body, body.chest]]) {
        if (!egg) continue;
        const turned = mq[bone].clone().multiply(restModel(skel, rest, bone).invert()), back = turned.clone().invert();
        const c = egg.off.clone().applyQuaternion(mq[bone]).add(mp[bone]);
        for (const pt of [mp[arm.hand], knuckles, mid(knuckles, tips), tips]) {
          const l = pt.clone().sub(c).applyQuaternion(back), skin = l.z > 0 ? frontOf(egg.front, l.x, l.y) : null;
          let out;
          if (skin !== null) {
            if (!(skin + thick > l.z)) continue;
            out = new THREE.Vector3(0, 0, skin + thick - l.z);
          } else {
            const k = Math.sqrt((l.x / egg.r.x) ** 2 + (l.y / egg.r.y) ** 2 + (l.z / egg.r.z) ** 2);
            if (k >= 1 || k < 1e-6) continue;
            out = l.clone().multiplyScalar(1 / k - 1);
          }
          out.applyQuaternion(turned);
          if (out.length() > most) { most = out.length(); push = out; }
        }
      }
      if (!push) return;
      wristTo(arm, mp[arm.hand].clone().add(push));
    }
  };
  // The fingers, where the tracker had the hand's own points: each of the character's fingers lies in its hand as the
  // dancer's does in theirs - joint to joint, the dancer's directions taken in the frame of their hand (wrist to
  // knuckles, first knuckle to little) and put in the frame of the character's. Which of the dancer's fingers: the
  // thumb the thumb's; the others in order from the first - three fingers (these characters): first, middle, little.
  // A finger of two bones: knuckle to middle joint, middle joint to tip; of one: knuckle to tip. A last bone has nothing
  // after it to say which way it points: it points the way it lies from its parent at rest. Without the hand's points
  // the fingers stay as they were the frame before.
  const handFrame = (lie, across) => {
    const z = lie.clone().normalize(), x = across.clone().addScaledVector(z, -across.dot(z)).normalize();
    return new THREE.Quaternion().setFromRotationMatrix(new THREE.Matrix4().makeBasis(x, z.clone().cross(x), z));
  };
  const restAt = b => {   // (a bone's place in the character's space at rest)
    const at = new THREE.Vector3();
    return at.setFromMatrixPosition(skel.bind[b]);
  };
  const fingers = (arm, g) => {
    if (opt.fingers === false || !arm.chains || !arm.chains.length || arm.index < 0) return;
    if (!g.full) { for (const c of arm.chains) for (const b of c.bones) keep(b); return; }
    const from = handFrame(mid(g.I, g.K).sub(g.W), g.I.clone().sub(g.K)).invert();
    const to = handFrame(knucklesOf(arm).sub(mp[arm.hand]), mp[arm.index].clone().sub(mp[arm.pinky])).multiply(from);
    for (const c of arm.chains) {
      const j = FINGER[c.finger], m = c.bones.length;
      // (the dancer's joints for each of the finger's bones: the thumb's first joint is in the palm - two bones start a joint on)
      const J = c.finger === 'thumb' && m < 3 ? [j + 1, j + 2, j + 3] : [j, j + 1, j + 2, j + 3];
      const ends = m === 1 ? [[J[0], J[J.length - 1]]] : m === 2 ? [[J[0], J[1]], [J[1], J[J.length - 1]]] : [[J[0], J[1]], [J[1], J[2]], [J[2], J[J.length - 1]]];
      for (let k = 0; k < Math.min(m, ends.length); k++) {
        const b = c.bones[k], want = V(g.base + ends[k][1]).sub(V(g.base + ends[k][0]));
        if (want.lengthSq() < 1e-10) continue;
        want.normalize().applyQuaternion(to);
        let now;
        if (k + 1 < m) now = mp[c.bones[k + 1]].clone().sub(mp[b]);
        else now = restAt(b).sub(restAt(parent[b])).applyQuaternion(restModel(skel, rest, b).invert()).applyQuaternion(mq[b]);
        if (now.lengthSq() < 1e-10) continue;
        turn(b, new THREE.Quaternion().setFromUnitVectors(now.normalize(), want));
      }
    }
  };
  const arm = (a, sh2, el, wr, index, pinky, base) => {
    const g = handOf(base, wr, index, pinky);
    aim(a.shoulder, a.elbow, sh2, el);
    aim(a.elbow, a.hand, el, wr);
    for (let pass = 0; pass < 2; pass++) { handAim(a, g); reach(a, g); }
    handAim(a, g);
    clear(a);
    handAim(a, g, true);
    fingers(a, g);
  };
  arm(A, L.shL, L.elL, L.wrL, L.indexL, L.pinkyL, HAND_L);
  arm(B, L.shR, L.elR, L.wrR, L.indexR, L.pinkyR, HAND_R);
  if (C) {
    aim(C.hip, C.knee, L.hipL, L.kneeL);
    aim(C.knee, C.ankle, L.kneeL, L.ankL);
    aim(C.ankle, C.toe, L.ankL, L.toeL);
    aim(D.hip, D.knee, L.hipR, L.kneeR);
    aim(D.knee, D.ankle, L.kneeR, L.ankR);
    aim(D.ankle, D.toe, L.ankR, L.toeR);
  }
  // The lower foot stays on the ground: the hips go up or down by what it's off. And the feet stay where they stand: with
  // the hips held in one place a dancer swaying over planted feet had the feet sliding under still hips - the hips go
  // sideways and fore and aft so the feet (the one on the ground, when one's lifted) are where they are at rest.
  if (C && opt.ground !== false) {
    const low = () => Math.min(...[C.ankle, C.toe, D.ankle, D.toe].filter(b => b >= 0).map(b => mp[b].y));
    const off = low() - opt.floor;
    const h = body.hips, shift = new THREE.Vector3(0, -off, 0);
    const lower = Math.min(mp[C.ankle].y, mp[D.ankle].y), lift = Math.max(1e-4, 0.08 * tall);
    const wL = 1 / (1 + ((mp[C.ankle].y - lower) / lift) ** 2), wR = 1 / (1 + ((mp[D.ankle].y - lower) / lift) ** 2);
    shift.x = (stood[0].x * wL + stood[1].x * wR - mp[C.ankle].x * wL - mp[D.ankle].x * wR) / (wL + wR);
    shift.z = (stood[0].z * wL + stood[1].z * wR - mp[C.ankle].z * wL - mp[D.ankle].z * wR) / (wL + wR);
    if (parent[h] >= 0) shift.applyQuaternion(mq[parent[h]].clone().invert());
    p[h].add(shift);
    fk();
  }
  return q.map((x, i) => ({q: x.toArray(), p: p[i].toArray()}));
}
// The dancer's head against their chest, from one frame's points: the chest's frame (shoulders across, hips below) and
// the head's turn from it - nod about x (down is more), turn about y, tilt about z. The head's frame is the ears across
// and the nose ahead of the middle of them: the tracker's face is squashed flat (eyes, nose and mouth within 2 cm of
// each other - up taken from the eyes over the mouth had every head looking 35 degrees down), so only those three.
// With the face's own points (the frame's three directions at HEAD - track's faceFrame: the eyes' outer corners across,
// chin to forehead up) the head's frame is those: `face` true, and nothing to take off - on the test clip a level head
// reads within a few degrees of level, where the ears and nose have it 17 up and 30 to one shoulder.
function headTurn(P, useFace = true) {
  const V = i => new THREE.Vector3(P[i].x, P[i].y, P[i].z), mid = (a, b) => a.add(b).multiplyScalar(0.5);
  const basis = (x, y, z) => new THREE.Quaternion().setFromRotationMatrix(new THREE.Matrix4().makeBasis(x, y, z));
  const face = useFace && P.length > HEAD + 2 && P[HEAD].v > 0 && P[HEAD + 1].v > 0;
  let head;
  if (face) {
    const x = V(HEAD).normalize(), up = V(HEAD + 1), y = up.addScaledVector(x, -up.dot(x)).normalize();
    head = basis(x, y, x.clone().cross(y));
  } else {
    const x = V(L.earL).sub(V(L.earR)).normalize(), ahead = V(L.nose).sub(mid(V(L.earL), V(L.earR)));
    const z = ahead.addScaledVector(x, -ahead.dot(x)).normalize();
    head = basis(x, z.clone().cross(x), z);
  }
  const cx = V(L.shL).sub(V(L.shR)).normalize(), up = mid(V(L.shL), V(L.shR)).sub(mid(V(L.hipL), V(L.hipR)));
  const cy = up.addScaledVector(cx, -up.dot(cx)).normalize(), chest = basis(cx, cy, cx.clone().cross(cy));
  return {chest, face, turn: new THREE.Euler().setFromQuaternion(chest.clone().invert().multiply(head), 'YXZ')};
}
// What the ears and nose read for a head held level and straight, to take off every frame that has no face (retarget's
// opt.headZero): how far they are from the face's own points, the middle one of the frames that have both. With no face
// in any frame: the nod and tilt of the clip's middle frame (a dancer looks ahead more than anywhere else) - on the test
// clip 17 degrees up and 30 to one shoulder, standing still facing the camera: the ears sit low and one is guessed.
export function headLevel(frames) {
  const nod = [], tilt = [], dNod = [], dTilt = [];
  for (const f of frames) {
    if (!f || Math.min(f[L.earL].v, f[L.earR].v, f[L.nose].v) < 0.3) continue;
    const e = headTurn(f, false).turn, both = headTurn(f);
    nod.push(e.x); tilt.push(e.z);
    if (both.face) { dNod.push(e.x - both.turn.x); dTilt.push(e.z - both.turn.z); }
  }
  const middle = a => a.sort((p, q) => p - q)[a.length >> 1];
  if (dNod.length >= 3) return {nod: middle(dNod), tilt: middle(dTilt)};
  if (nod.length < 3) return {nod: 0, tilt: 0};
  return {nod: middle(nod), tilt: middle(tilt)};
}
// a bone's turn in the character's space, at rest
function restModel(skel, rest, b) {
  const out = new THREE.Quaternion().fromArray(rest[b].q);
  for (let i = skel.parent[b]; i >= 0; i = skel.parent[i]) out.premultiply(new THREE.Quaternion().fromArray(rest[i].q));
  return out;
}
// The character's shape, for hands put against it and kept out of it (retarget's reach and clear), from its vertices at
// rest: `head` - an egg about what the head bone and what's on it move (its middle from the bone, in the bone's axes; its
// three half sizes across, up and deep); `face` - between its eyes: the middle of the joints on the head in its upper
// part (Lois's two eye joints; Peter's ten of eyes, lids and brows), from the bone, or the front of the egg when it has
// none; `body` - an egg about what the spine's bones move (hips to chest), on the chest bone.
export function shapeOf(skel, body, meshes) {
  const n = skel.count, inHead = new Array(n).fill(false), inBody = new Array(n).fill(false);
  inHead[body.head] = true;
  for (let i = 0; i < n; i++) if (skel.parent[i] >= 0 && inHead[skel.parent[i]]) inHead[i] = true;   // (parents come first)
  for (let b = body.chest; b >= 0; b = skel.parent[b]) { inBody[b] = true; if (b === body.hips) break; }
  for (let b = skel.parent[body.head]; b >= 0 && b !== body.chest; b = skel.parent[b]) inBody[b] = true;   // (the neck)
  const boxes = [new THREE.Box3(), new THREE.Box3()], own = [[], []], v = new THREE.Vector3();
  for (const m of meshes) {
    const pos = m.geometry.attributes.position, si = m.geometry.attributes.skinIndex, sw = m.geometry.attributes.skinWeight;
    if (!pos || !si || !sw) continue;
    for (let i = 0; i < pos.count; i++) {
      let h = 0, t = 0;
      for (let k = 0; k < 4; k++) { const b = si.getComponent(i, k), w = sw.getComponent(i, k); if (inHead[b]) h += w; else if (inBody[b]) t += w; }
      const part = h > 0.5 ? 0 : t > 0.6 ? 1 : -1;
      if (part < 0) continue;
      boxes[part].expandByPoint(v.fromBufferAttribute(pos, i));
      own[part].push(v.x, v.y, v.z);
    }
  }
  if (boxes[0].isEmpty()) return null;
  // (.front: how far ahead of the part's middle its surface is, over its width and height - a grid, each square the
  // furthest-forward vertex in it or beside it; -Infinity where there's nothing)
  const onBone = (bone, part, grow) => {
    const box = boxes[part], at = new THREE.Vector3(), turn = new THREE.Quaternion(), size = new THREE.Vector3();
    skel.bind[bone].decompose(at, turn, size);
    const c = box.getCenter(new THREE.Vector3()), half = box.getSize(new THREE.Vector3()).multiplyScalar(0.5);
    const N = 20, raw = new Float32Array(N * N).fill(-Infinity), z = new Float32Array(N * N).fill(-Infinity), pts = own[part];
    const cell = (x, h) => Math.max(0, Math.min(N - 1, Math.floor((x / h + 1) / 2 * N)));
    for (let i = 0; i < pts.length; i += 3) {
      const k = cell(pts[i + 1] - c.y, half.y) * N + cell(pts[i] - c.x, half.x);
      raw[k] = Math.max(raw[k], pts[i + 2] - c.z);
    }
    for (let y = 0; y < N; y++) for (let x = 0; x < N; x++)
      for (let dy = -1; dy <= 1; dy++) for (let dx = -1; dx <= 1; dx++) {
        const yy = y + dy, xx = x + dx;
        if (yy >= 0 && yy < N && xx >= 0 && xx < N) z[y * N + x] = Math.max(z[y * N + x], raw[yy * N + xx]);
      }
    return {off: c.clone().sub(at).applyQuaternion(turn.invert()), r: half.clone().multiplyScalar(grow), front: {n: N, half: [half.x, half.y], z}};
  };
  const head = onBone(body.head, 0, 1.0);
  const at = new THREE.Vector3(), turn = new THREE.Quaternion(), size = new THREE.Vector3();
  skel.bind[body.head].decompose(at, turn, size);
  const joints = [];
  for (let i = 0; i < n; i++) if (i !== body.head && inHead[i]) joints.push(new THREE.Vector3().setFromMatrixPosition(skel.bind[i]));
  const top = Math.max(0, ...joints.map(j => j.y - at.y)), eyes = joints.filter(j => top > 1e-3 && j.y - at.y >= 0.6 * top);
  const mid = boxes[0].getCenter(new THREE.Vector3());
  const facePoint = eyes.length ? eyes.reduce((s2, j) => s2.add(j), new THREE.Vector3()).multiplyScalar(1 / eyes.length) : new THREE.Vector3(mid.x, mid.y, boxes[0].max.z);
  facePoint.x = mid.x;   // (between them)
  // (up, down: from the eyes to the top of the head, and to its underside)
  const up = Math.max(1e-3, boxes[0].max.y - facePoint.y), down = Math.max(1e-3, facePoint.y - boxes[0].min.y);
  return {head, up, down, face: facePoint.sub(at).applyQuaternion(turn.clone().invert()), body: boxes[1].isEmpty() ? null : onBone(body.chest, 1, 0.9)};
}
// A part's front at (x, y) from its middle (shapeOf's .front): how far ahead its surface is there, between the squares'
// middles; null outside its width and height, or where it has no surface.
function frontOf(front, x, y) {
  const n = front.n, u = (x / front.half[0] + 1) / 2 * n - 0.5, v = (y / front.half[1] + 1) / 2 * n - 0.5;
  if (u < -0.5 || v < -0.5 || u > n - 0.5 || v > n - 0.5) return null;
  const x0 = Math.max(0, Math.min(n - 1, Math.floor(u))), y0 = Math.max(0, Math.min(n - 1, Math.floor(v)));
  const x1 = Math.min(n - 1, x0 + 1), y1 = Math.min(n - 1, y0 + 1), fx = Math.max(0, Math.min(1, u - x0)), fy = Math.max(0, Math.min(1, v - y0));
  const at = [front.z[y0 * n + x0], front.z[y0 * n + x1], front.z[y1 * n + x0], front.z[y1 * n + x1]], real = at.filter(Number.isFinite);
  if (!real.length) return null;
  const least = Math.min(...real), [a, b, c, d] = at.map(q => (Number.isFinite(q) ? q : least));   // (a square with nothing: as the lowest beside it)
  return (a * (1 - fx) + b * fx) * (1 - fy) + (c * (1 - fx) + d * fx) * fy;
}
// how low the feet are at rest (the floor)
export function floorOf(skel, body, rest) {
  const n = skel.count, mq = [], mp = [];
  for (let i = 0; i < n; i++) {
    const q = new THREE.Quaternion().fromArray(rest[i].q), p = new THREE.Vector3().fromArray(rest[i].p), par = skel.parent[i];
    if (par < 0) { mq[i] = q; mp[i] = p; } else { mq[i] = mq[par].clone().multiply(q); mp[i] = p.applyQuaternion(mq[par]).add(mp[par]); }
  }
  if (!body.legL) return 0;
  return Math.min(...[body.legL.ankle, body.legL.toe, body.legR.ankle, body.legR.toe].filter(b => b >= 0).map(b => mp[b].y));
}

// Frames the tracker got wrong, or has nobody in (null). From the front a body can be read as seen from behind, left and
// right changed over, or lying down - for a frame or a run of them, where it's unsure (a pose with the arms in front of
// the body: the test clip's first twenty frames). Two things a body can't do between two frames: turn its chest more than
// a quarter round, or go from upright to lying. A frame that does either against the last good one is left out - unless
// the frames after it stay that way (a real spin, a real dive, goes there through the frames between). Where to start:
// the frame of the first second that's most upright and most toward the camera (the dancer's asked to face it).
// What's left out is filled from the good frames either side.
export function unflip(frames) {
  const V = p => new THREE.Vector3(p.x, p.y, p.z);
  const look = f => {
    if (!f) return null;
    const across = V(f[L.shL]).sub(V(f[L.shR])), up = V(f[L.shL]).add(V(f[L.shR])).sub(V(f[L.hipL])).sub(V(f[L.hipR]));
    if (up.lengthSq() < 1e-10) return null;
    const z = across.cross(up), upright = up.y / up.length();
    z.y = 0;
    return z.lengthSq() > 1e-10 ? {face: z.normalize(), upright} : null;
  };
  const at = frames.map(look), n = frames.length, good = new Array(n).fill(true);
  let first = -1, best = -Infinity;
  for (let i = 0; i < Math.min(n, 30); i++) {
    if (!at[i]) continue;
    const score = at[i].upright + 0.5 * at[i].face.z;
    if (score > best) { best = score; first = i; }
  }
  if (first < 0) return frames;
  const fits = (a, b) => a.face.dot(b.face) >= 0 && !(b.upright > 0.7 && a.upright < 0.35);
  // forward from there, then back from there
  for (const step of [1, -1]) {
    let last = at[first];
    for (let i = first + step; i >= 0 && i < n; i += step) {
      const a = at[i];
      if (!a) { good[i] = false; continue; }
      if (fits(a, last)) { last = a; continue; }
      let stays = 0;
      for (let k = 1; k <= 8; k++) { const b = at[i + k * step]; if (b && b.face.dot(a.face) > 0.5 && Math.abs(b.upright - a.upright) < 0.3) stays++; }
      if (stays >= 7 && a.upright >= 0.35) { last = a; continue; }
      good[i] = false;
    }
  }
  if (good.every(Boolean)) return frames;
  return frames.map((f, i) => {
    if (good[i]) return f;
    let a = i - 1, b = i + 1;
    while (a >= 0 && !good[a]) a--;
    while (b < n && !good[b]) b++;
    if (a < 0 && b >= n) return f;
    if (a < 0) return frames[b].map(p => ({...p}));
    if (b >= n) return frames[a].map(p => ({...p}));
    const k = (i - a) / (b - a);
    return frames[a].map((_, j) => ({x: frames[a][j].x + (frames[b][j].x - frames[a][j].x) * k, y: frames[a][j].y + (frames[b][j].y - frames[a][j].y) * k,
      z: frames[a][j].z + (frames[b][j].z - frames[a][j].z) * k, v: Math.min(frames[a][j].v, frames[b][j].v)}));   // (a hand or a face only one of them has: none - v 0)
  });
}

// The camera's tilt taken out. The tracker's points are in the camera's axes, not the room's: filmed from a camera that
// looks a little down (most are - the test clip's by 10 degrees), a dancer standing straight is read as leaning that
// much toward it, and the character danced bent forward, looking at the floor. A dancer stands straight more than
// anything else: the lean of the body (ankles to shoulders; hips to shoulders where the legs aren't seen) toward or away
// from the camera, the middle one of all the frames, is turned out of every point.
export function upright(frames) {
  const lean = [];
  for (const f of frames) {
    if (!f) continue;
    const legs = Math.min(f[L.ankL].v, f[L.ankR].v) >= 0.5, low = legs ? [L.ankL, L.ankR] : [L.hipL, L.hipR];
    const y = f[L.shL].y + f[L.shR].y - f[low[0]].y - f[low[1]].y, z = f[L.shL].z + f[L.shR].z - f[low[0]].z - f[low[1]].z;
    if (y > 0 && y > Math.abs(z)) lean.push(Math.atan2(z, y));
  }
  if (lean.length < 3) return frames;
  lean.sort((a, b) => a - b);
  const a = Math.max(-0.5, Math.min(0.5, lean[lean.length >> 1]));
  if (Math.abs(a) < 0.02) return frames;
  const c = Math.cos(a), s = Math.sin(a);
  return frames.map(f => f && f.map(p => ({...p, y: p.y * c + p.z * s, z: p.z * c - p.y * s})));
}

// Some bones' turns eased over the keys either side (half this key's, a quarter of each neighbour's). The arms: a hand
// put against the face, kept out of the head and turned palm-on is worked out one frame at a time, and where one frame's
// answer differs from the next the hand would twitch.
export function easeKeys(keys, bones) {
  const was = keys.map(k => bones.map(b => k.pose[b].q.slice()));
  for (let i = 0; i < keys.length; i++) bones.forEach((b, j) => {
    const q = was[i][j], sum = q.map(v => 2 * v);
    for (const n of [i - 1, i + 1]) {
      const o = was[n] ? was[n][j] : q, s = o[0] * q[0] + o[1] * q[1] + o[2] * q[2] + o[3] * q[3] < 0 ? -1 : 1;
      for (let k = 0; k < 4; k++) sum[k] += s * o[k];
    }
    const len = Math.hypot(...sum) || 1;
    keys[i].pose[b].q = sum.map(v => v / len);
  });
}

// The points over time, steadied: each point's place averaged with its neighbours in time (wider where it's less sure).
export function smooth(frames, width = 2) {
  return frames.map((f, i) => f.map((pt, j) => {
    if (j >= HAND_L && !(pt.v > 0)) return pt;   // (a hand or a face the tracker didn't have)
    let x = 0, y = 0, z = 0, w = 0;
    for (let k = -width; k <= width; k++) {
      const g = frames[i + k];
      if (!g || !g[j] || (j >= HAND_L && !(g[j].v > 0))) continue;
      const wt = (width + 1 - Math.abs(k)) * (0.05 + g[j].v);
      x += g[j].x * wt; y += g[j].y * wt; z += g[j].z * wt; w += wt;
    }
    return {x: x / w, y: y / w, z: z / w, v: pt.v};
  }));
}

// A hand the tracker read wrong is left out (as if it had none). Over a face it can read a hand back to front - on the
// test clip the hand over the eyes pointing up and outward for two frames, where the arm and the body's points have it
// lying across the face; filled between, the arm stood straight up. Neither check does alone: the body's three hand
// points are rough (40 degrees off a hand that plainly carries on from its forearm, for seconds), and a wrist does
// bend. So: out when the way the hand lies (wrist to knuckles) is more than 45 degrees from the forearm's AND more than
// 45 from the body's points' (or those aren't sure). Then, of what's left: out when it lies more than 40 degrees from
// the hands of the eight frames either side taken together (two more frames of that clip passed the first check and
// pointed up between frames that lay across).
function checkHands(frames) {
  const dir = (f, a, b) => new THREE.Vector3(f[a].x - f[b].x, f[a].y - f[b].y, f[a].z - f[b].z);
  const far = (a, b, most) => a.lengthSq() < 1e-12 || b.lengthSq() < 1e-12 || a.angleTo(b) > most;
  const lieOf = (f, base) => dir(f, base + FINGER.index, base).add(dir(f, base + FINGER.pinky, base));
  const drop = (out, i, base) => {
    if (out[i] === frames[i]) out[i] = frames[i].slice();
    for (let j = 0; j < 21; j++) out[i][base + j] = {...frames[i][base + j], v: 0};
  };
  const out = frames.slice();
  for (const [base, wr, el, ix, pk] of [[HAND_L, L.wrL, L.elL, L.indexL, L.pinkyL], [HAND_R, L.wrR, L.elR, L.indexR, L.pinkyR]]) {
    const has = i => out[i] && out[i].length >= POINTS && out[i][base].v > 0;
    for (let i = 0; i < out.length; i++) {
      if (!has(i)) continue;
      const f = out[i], lie = lieOf(f, base), body = dir(f, ix, wr).add(dir(f, pk, wr)), rough = Math.min(f[wr].v, f[ix].v, f[pk].v) < 0.2;
      if ((rough || far(lie, body, Math.PI / 4)) && far(lie, dir(f, wr, el), Math.PI / 4)) drop(out, i, base);
    }
    const odd = [];
    for (let i = 0; i < out.length; i++) {
      if (!has(i)) continue;
      const near = new THREE.Vector3();
      let count = 0;
      for (let k = -8; k <= 8; k++) if (k && has(i + k)) { near.add(lieOf(out[i + k], base).normalize()); count++; }
      if (count >= 2 && far(lieOf(out[i], base), near, 40 * Math.PI / 180)) odd.push(i);
    }
    for (const i of odd) drop(out, i, base);
  }
  return out;
}

// A hand or a face the tracker lost for a moment (a hand going over the face, a face behind a hand - on the test clip a
// hand for up to 15 frames, the face for 6): filled from the frames either side, for gaps of up to `most` frames. A
// hand's points go between the two as they lie from their wrist, then onto this frame's wrist; the head's directions
// between the two. A longer gap stays empty: the hand is turned from the body's three points there, the head from the
// ears and nose.
export function fillGaps(frames, most = 24) {
  frames = checkHands(frames);
  const out = frames.map(f => f && f.map(p => ({...p})));
  const groups = [[HAND_L, 21, L.wrL], [HAND_R, 21, L.wrR], [HEAD, 3, -1]];
  for (const [base, count, wrist] of groups) {
    const has = i => out[i] && out[i].length > base && frames[i][base].v > 0;
    let last = -1;
    for (let i = 0; i < out.length; i++) {
      if (!has(i)) continue;
      if (last >= 0 && i - last > 1 && i - last - 1 <= most) {
        for (let m = last + 1; m < i; m++) {
          if (!out[m] || out[m].length <= base) continue;
          const k = (m - last) / (i - last), A = frames[last], B = frames[i], at = wrist < 0 ? null : out[m][wrist];
          for (let j = 0; j < count; j++) {
            const a = A[base + j], b = B[base + j], oa = wrist < 0 ? {x: 0, y: 0, z: 0} : A[wrist], ob = wrist < 0 ? {x: 0, y: 0, z: 0} : B[wrist];
            out[m][base + j] = {x: (a.x - oa.x) * (1 - k) + (b.x - ob.x) * k + (at ? at.x : 0), y: (a.y - oa.y) * (1 - k) + (b.y - ob.y) * k + (at ? at.y : 0),
              z: (a.z - oa.z) * (1 - k) + (b.z - ob.z) * k + (at ? at.z : 0), v: Math.min(a.v, b.v)};
          }
        }
      }
      last = i;
    }
  }
  return out;
}

export function createMocap({$, setStatus, editor}) {
  const mc = {video: null, url: '', landmarker: null, frames: [], times: [], busy: false, mirror: false};
  const FPS = 30;

  function setVideo(src, name) {
    if (mc.url && mc.url.startsWith('blob:')) URL.revokeObjectURL(mc.url);
    mc.url = src;
    const v = $('mocapVideo');
    v.src = src;
    v.hidden = false;
    $('mocapOverlay').hidden = false;
    mc.frames = [];
    $('mocapUse').disabled = true;
    v.onloadedmetadata = () => {
      const len = Math.min(6, v.duration || 6);
      $('mocapStart').value = 0;
      $('mocapLength').value = len.toFixed(1);
      $('mocapTrack').disabled = false;
      if (!$('emoteName').value) $('emoteName').value = (name || 'Dance').replace(/\.[^.]+$/, '').replace(/[^\w\- ]+/g, ' ').trim().slice(0, 40);
      setStatus(`"${name}" - ${v.duration.toFixed(1)} s. Pick the part to use, then Track the dance.`);
    };
    v.onerror = () => setStatus("The browser can't play that video - try an .mp4 or .webm.");
  }
  $('mocapFile').onchange = e => {
    const f = e.target.files[0];
    e.target.value = '';
    if (f) setVideo(URL.createObjectURL(f), f.name);
  };

  // The trackers: the body's (the pose landmarker's heaviest model - the best at how far forward a limb is) and, for the
  // hands and the face, the holistic one (its own body model is a lighter one: only its hands and face are used). If the
  // second can't be had the body's does alone, as before it.
  async function landmarker() {
    if (mc.landmarker) return mc.landmarker;
    setStatus('Getting the trackers (about 45 MB, the first time)...');
    const vision = await import(/* @vite-ignore */ VISION);
    const files = await vision.FilesetResolver.forVisionTasks(VISION + '/wasm');
    const make = delegate => vision.PoseLandmarker.createFromOptions(files, {
      baseOptions: {modelAssetPath: MODEL, delegate}, runningMode: 'VIDEO', numPoses: 1,
      minPoseDetectionConfidence: 0.4, minPosePresenceConfidence: 0.4, minTrackingConfidence: 0.4});
    try { mc.landmarker = await make('GPU'); } catch (e) { mc.landmarker = await make('CPU'); }
    const whole = delegate => vision.HolisticLandmarker.createFromOptions(files, {
      baseOptions: {modelAssetPath: HOLISTIC, delegate}, runningMode: 'VIDEO',
      minPoseDetectionConfidence: 0.4, minPosePresenceConfidence: 0.4, minHandLandmarksConfidence: 0.3,
      minFaceDetectionConfidence: 0.3, minFacePresenceConfidence: 0.3});
    try { mc.whole = await whole('GPU'); } catch (e) { try { mc.whole = await whole('CPU'); } catch (e2) { mc.whole = null; } }
    return mc.landmarker;
  }
  // One frame's points past the body's (the note at HAND_L): `body` the frame's 33 so far (the hands are put on its
  // wrists), `im` the body's points in the picture (which wrist a hand is nearest says whose it is), `r` the holistic
  // tracker's result, `W` and `H` the picture's size (the face's points are in parts of its width and height).
  function extras(body, im, r, W, H) {
    const none = () => ({x: 0, y: 0, z: 0, v: 0});
    const out = body.slice();
    for (let j = 33; j < POINTS; j++) out[j] = none();
    if (!r) return out;
    const hands = [[r.leftHandLandmarks, r.leftHandWorldLandmarks, HAND_L, L.wrL], [r.rightHandLandmarks, r.rightHandWorldLandmarks, HAND_R, L.wrR]];
    for (const [pic, world, base, wrist] of hands) {
      const p = pic && pic[0], w = world && world[0];
      if (!p || !w || p.length < 21 || w.length < 21) continue;
      // (a hand given as the other one's: its wrist in the picture is at the other wrist)
      const other = wrist === L.wrL ? L.wrR : L.wrL, d = k => Math.hypot((p[0].x - im[k].x) * W, (p[0].y - im[k].y) * H);
      if (d(other) < d(wrist) * 0.5) continue;
      const at = body[wrist];
      for (let j = 0; j < 21; j++) out[base + j] = {x: at.x + w[j].x - w[0].x, y: at.y - (w[j].y - w[0].y), z: at.z - (w[j].z - w[0].z), v: 1};
    }
    const f = r.faceLandmarks && r.faceLandmarks[0];
    if (f && f.length > 263) {
      // the face's own frame: the eyes' outer corners across (33 its right, 263 its left), chin (152) to forehead (10) up
      const c = i => new THREE.Vector3(f[i].x * W, -f[i].y * H, -f[i].z * W);
      const x = c(263).sub(c(33)), up = c(10).sub(c(152));
      if (x.lengthSq() > 1e-6 && up.lengthSq() > 1e-6) {
        x.normalize();
        const y = up.addScaledVector(x, -up.dot(x)).normalize(), z = x.clone().cross(y);
        [x, y, z].forEach((d, j) => { out[HEAD + j] = {x: d.x, y: d.y, z: d.z, v: 1}; });
      }
    }
    return out;
  }
  const seekTo = (v, t) => new Promise(done => {
    if (Math.abs(v.currentTime - t) < 1e-4 && v.readyState >= 2) { done(); return; }
    v.addEventListener('seeked', () => done(), {once: true});
    v.currentTime = t;
  });

  function drawOverlay(lm, hands) {
    const c = $('mocapOverlay'), v = $('mocapVideo');
    c.width = v.clientWidth; c.height = v.clientHeight;
    const g = c.getContext('2d');
    g.clearRect(0, 0, c.width, c.height);
    if (!lm || !$('mocapShow').checked) return;
    // (the video is fitted inside its box: where its picture is)
    const s = Math.min(c.width / v.videoWidth, c.height / v.videoHeight), w = v.videoWidth * s, h = v.videoHeight * s;
    const ox = (c.width - w) / 2, oy = (c.height - h) / 2;
    g.lineWidth = 2;
    g.strokeStyle = '#ffcf33';
    for (const [a, b] of LINES) {
      if (lm[a].visibility < 0.4 || lm[b].visibility < 0.4) continue;
      g.beginPath(); g.moveTo(ox + lm[a].x * w, oy + lm[a].y * h); g.lineTo(ox + lm[b].x * w, oy + lm[b].y * h); g.stroke();
    }
    g.fillStyle = '#5cc8ff';
    for (const i of Object.values(L)) {
      if (lm[i].visibility < 0.4) continue;
      g.beginPath(); g.arc(ox + lm[i].x * w, oy + lm[i].y * h, 3, 0, 7); g.fill();
    }
    // (the hands' points, where the tracker has them)
    g.fillStyle = '#7dff9a';
    for (const hand of hands || []) for (const p of hand || []) { g.beginPath(); g.arc(ox + p.x * w, oy + p.y * h, 2, 0, 7); g.fill(); }
  }

  async function track() {
    const rig = editor.rig;
    if (mc.busy || !mc.url) return;
    if (!rig) { setStatus('Pick a character first.'); return; }
    const body = mapBody(rig.skel);
    if (!body) { setStatus("Couldn't find this character's arms and legs in its skeleton - try another character."); return; }
    mc.busy = true;
    $('mocapTrack').disabled = true;
    const v = $('mocapVideo');
    try {
      const lmk = await landmarker();
      v.pause();
      const start = Math.max(0, +$('mocapStart').value || 0), len = Math.max(0.2, Math.min(20, +$('mocapLength').value || 6));
      const end = Math.min(v.duration || start + len, start + len), count = Math.max(2, Math.round((end - start) * FPS) + 1);
      mc.frames = []; mc.image = []; mc.times = [];
      let seen = 0, stamp = performance.now();
      for (let i = 0; i < count; i++) {
        const t = Math.min(end, start + i / FPS);
        await seekTo(v, t);
        stamp += 1000 / FPS;   // (the tracker wants times that only go up)
        const r = lmk.detectForVideo(v, stamp);
        const w = r.worldLandmarks && r.worldLandmarks[0], im = r.landmarks && r.landmarks[0];
        let more = null;
        if (w && im && mc.whole) { try { more = mc.whole.detectForVideo(v, stamp); } catch (e) { more = null; } }
        if (w) seen++;
        mc.frames.push(w ? extras(w.map(p => ({x: p.x, y: -p.y, z: -p.z, v: p.visibility ?? 1})), im, more, v.videoWidth, v.videoHeight) : null);
        mc.image.push(im || null);
        mc.times.push(t - start);
        drawOverlay(im, more && [more.leftHandLandmarks && more.leftHandLandmarks[0], more.rightHandLandmarks && more.rightHandLandmarks[0]]);
        $('mocapBar').style.width = Math.round((i + 1) / count * 100) + '%';
        if (i % 10 === 0) setStatus(`Tracking... ${Math.round((i + 1) / count * 100)}%`);
      }
      if (seen < count * 0.5) throw new Error('no person was found in most of it - the whole body has to be in view');
      apply(rig, body);   // (frames with nobody stay empty: filled from the frames either side - unflip)
      $('mocapUse').disabled = false;
      setStatus(`Tracked ${seen} of ${count} frames - the character is doing it now. Save emote keeps it.`);
    } catch (err) {
      setStatus("Couldn't track that video: " + (err.message || err));
    }
    mc.busy = false;
    $('mocapTrack').disabled = false;
  }

  // the tracked points onto the character's bones: keys on the emote's timeline
  function apply(rig, body) {
    if (!mc.frames.length) return;
    body = body || mapBody(rig.skel);
    const mirror = $('mocapMirror').checked;
    let pts = mc.frames;
    if (mirror) {   // (a mirrored video: left and right change places)
      const swap = [0, 4, 5, 6, 1, 2, 3, 8, 7, 10, 9, 12, 11, 14, 13, 16, 15, 18, 17, 20, 19, 22, 21, 24, 23, 26, 25, 28, 27, 30, 29, 32, 31];
      for (let j = 0; j < 21; j++) { swap[HAND_L + j] = HAND_R + j; swap[HAND_R + j] = HAND_L + j; }
      for (let j = 0; j < 3; j++) swap[HEAD + j] = HEAD + j;
      // (the head's direction across is to its own left: mirrored and turned about, it's the same again)
      pts = pts.map(f => f && f.map((_, j) => (j < f.length && swap[j] < f.length ? {...f[swap[j]], x: j === HEAD ? f[j].x : -f[swap[j]].x, y: j === HEAD ? -f[j].y : f[swap[j]].y,
        z: j === HEAD ? -f[j].z : f[swap[j]].z} : f[j])));
    }
    pts = unflip(pts);
    if (!pts.some(f => !f)) pts = fillGaps(pts);
    if (pts.some(f => !f)) { setStatus("The tracker didn't get a steady look at the dancer - try another part of the video."); return; }
    if ($('mocapLevel').checked) pts = upright(pts);
    pts = smooth(pts, +$('mocapSmooth').value);
    const floor = floorOf(rig.skel, body, rig.rest), keys = [];
    const shape = $('mocapReach').checked || $('mocapClip').checked ? shapeOf(rig.skel, body, editor.meshes) : null;
    const headZero = $('mocapLevel').checked ? headLevel(pts) : null;
    let prev = null;
    const step = 2;   // (a key every other frame: 15 a second)
    for (let i = 0; i < pts.length; i += step) {
      const pose = retarget(rig.skel, body, rig.rest, pts[i], prev, {floor, ground: $('mocapGround').checked, shape, near: $('mocapReach').checked ? undefined : -1, far: $('mocapReach').checked ? undefined : 0,
        clip: $('mocapClip').checked, headZero, hands: $('mocapHands').checked, fingers: $('mocapHands').checked, face: $('mocapHands').checked});
      keys.push({t: Math.round(mc.times[i] * 30) / 30, pose});
      prev = pose;
    }
    if (shape) easeKeys(keys, [body.armL, body.armR].flatMap(a => [a.shoulder, a.elbow, a.hand, ...(a.chains || []).flatMap(c => c.bones)]));
    editor.setKeys(keys, mc.times[mc.times.length - 1]);
  }

  $('mocapTrack').onclick = track;
  $('mocapUse').onclick = () => { if (editor.rig) apply(editor.rig); };
  for (const id of ['mocapMirror', 'mocapGround', 'mocapSmooth', 'mocapReach', 'mocapClip', 'mocapLevel', 'mocapHands']) $(id).onchange = () => { if (editor.rig && mc.frames.length) apply(editor.rig); };
  $('mocapShow').onchange = () => drawOverlay(null);

  return {
    setVideo, track,
    get frames() { return mc.frames; },
    // (testing: points made by hand, as if tracked)
    usePoints(frames, times) { mc.frames = frames; mc.times = times; if (editor.rig) apply(editor.rig); },
  };
}
