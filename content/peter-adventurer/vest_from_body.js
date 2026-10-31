// The vest made from Peter himself (run in the Skin Studio's page, index.html?dev):
//   (await import('/content/peter-adventurer/vest_from_body.js')).build()
// The first vest (img2threejs: an implicit shell, meshed coarsely) had stair-stepped edges and straps and sat up to 0.1
// inside him. This one is his own surface: his body and shirt (welded into one mesh), the part of it the first vest
// covered (each of his points: does the first vest lie over it, along his normal - that coverage smoothed over his
// surface, so the outline - front edges, hem, armholes, collar line - keeps the first vest's shape, as smooth curves),
// cut along that outline through his triangles, lifted GAP off him with THICK of leather (an outer and an inner
// surface and the rim between them round every edge). Every point of it lies over one of his: it bends exactly as he
// does. The pockets (each with its flap and button) and the front buttons are seated on it again. Written as
// build/research/peter_adventurer_outfit_v2.glb - the outfit studio_build.js puts on him.
import * as THREE from 'three';
import {GLTFExporter} from 'three/addons/exporters/GLTFExporter.js';
import {load, bodyGrid, outOf} from './clash_check.js';

const GAP = 0.02, THICK = 0.016, SMOOTH = 8;

// his surface: the pieces on his body and clothes textures, welded where pieces meet (their normals averaged)
// (arm: how much a point follows his arms - the bones past the collarbone - 0 to 1)
function bodyMesh(pieces, names) {
  const armBone = names.map(n => /^(Arm [LR] [2-6] |Hand |Finger )/.test(n));
  const key = (x, y, z) => Math.round(x * 1e4) + ',' + Math.round(y * 1e4) + ',' + Math.round(z * 1e4);
  const at = new Map(), pos = [], nrm = [], tris = [], arm = [], seen = [];
  for (const p of pieces) {
    if (!/_Char_CO|Damage/i.test(p.texture?.name || '')) continue;
    const map = [];
    for (let i = 0; i < p.position.length / 3; i++) {
      const k = key(p.position[i * 3], p.position[i * 3 + 1], p.position[i * 3 + 2]);
      let j = at.get(k);
      if (j === undefined) {
        j = pos.length;
        at.set(k, j);
        pos.push(new THREE.Vector3().fromArray(p.position, i * 3));
        nrm.push(new THREE.Vector3());
        arm.push(0); seen.push(0);
      }
      nrm[j].add(new THREE.Vector3().fromArray(p.normal, i * 3));
      if (p.skin) {
        let a = 0, t = 0;
        for (let k = 0; k < 4; k++) { const w = p.skin.weight[i * 4 + k]; t += w; if (armBone[p.skin.index[i * 4 + k]]) a += w; }
        if (t) { arm[j] += a / t; seen[j]++; }
      }
      map.push(j);
    }
    for (let t = 0; t < p.index.length; t += 3) {
      const a = map[p.index[t]], b = map[p.index[t + 1]], c = map[p.index[t + 2]];
      if (a !== b && b !== c && a !== c) tris.push([a, b, c]);
    }
  }
  nrm.forEach(n => n.normalize());
  return {pos, nrm, tris, arm: arm.map((a, j) => seen[j] ? a / seen[j] : 0)};
}

// which of his points the first vest lies over (from 0.2 inside him to 0.25 out, along his normal)
function coverage(body, vestGeo) {
  const mesh = new THREE.Mesh(vestGeo, new THREE.MeshBasicMaterial({side: THREE.DoubleSide}));
  const ray = new THREE.Raycaster();
  return body.pos.map((p, i) => {
    ray.set(p.clone().addScaledVector(body.nrm[i], -0.2), body.nrm[i]);
    ray.far = 0.45;
    return ray.intersectObject(mesh, false).length ? 1 : 0;
  });
}
// The vest's outline as smooth curves: the coverage spread into a 3D grid (CELL), blurred (BLUR across, the same every
// way - unlike his triangles, big and small), and read back at each of his points as a weighted average of the
// coverage near it. In 3D, not on a map of him: the top of his shoulder and his side under the arm lie together on any
// map round his torso, not in space. His arms (the points following his arm bones more than ARMS) are no part of it:
// the armholes a clean cut there - by his bones, not how far out a point is (his belly is as wide as his shoulders).
const ARMS = 0.5, BLUR = 0.035, CELL = 0.015;
function outline(body, cov) {
  const box = new THREE.Box3();
  body.pos.forEach((p, i) => { if (body.arm[i] < 0.9) box.expandByPoint(p); });
  box.expandByScalar(BLUR * 3);
  const n = box.getSize(new THREE.Vector3()).divideScalar(CELL).ceil().addScalar(1), N = [n.x, n.y, n.z];
  const S = new Float32Array(N[0] * N[1] * N[2]), W = new Float32Array(S.length);
  const idx = (x, y, z) => (z * N[1] + y) * N[0] + x;
  const cellOf = p => [(p.x - box.min.x) / CELL, (p.y - box.min.y) / CELL, (p.z - box.min.z) / CELL];
  body.pos.forEach((p, i) => {   // (splatted: trilinear)
    if (body.arm[i] >= 0.9 || !box.containsPoint(p)) return;
    const c = cellOf(p), c0 = c.map(Math.floor), fr = c.map((v, k) => v - c0[k]);
    for (let dz = 0; dz < 2; dz++) for (let dy = 0; dy < 2; dy++) for (let dx = 0; dx < 2; dx++) {
      const w = (dx ? fr[0] : 1 - fr[0]) * (dy ? fr[1] : 1 - fr[1]) * (dz ? fr[2] : 1 - fr[2]), k = idx(c0[0] + dx, c0[1] + dy, c0[2] + dz);
      S[k] += cov[i] * w; W[k] += w;
    }
  });
  const sigma = BLUR / CELL, R = Math.ceil(sigma * 3), ker = [];
  for (let k = -R; k <= R; k++) ker.push(Math.exp(-k * k / (2 * sigma * sigma)));
  const blur = (A, axis) => {   // (a Gaussian along one axis)
    const out = new Float32Array(A.length), st = [1, N[0], N[0] * N[1]][axis];
    for (let z = 0; z < N[2]; z++) for (let y = 0; y < N[1]; y++) for (let x = 0; x < N[0]; x++) {
      const at = [x, y, z][axis], base = idx(x, y, z);
      let s = 0;
      for (let j = 0; j < ker.length; j++) { const q = at + j - R; if (q >= 0 && q < N[axis]) s += A[base + (j - R) * st] * ker[j]; }
      out[base] = s;
    }
    return out;
  };
  let Sb = S, Wb = W;
  for (const axis of [0, 1, 2]) { Sb = blur(Sb, axis); Wb = blur(Wb, axis); }
  const read = (A, p) => {   // (trilinear)
    const c = cellOf(p), c0 = c.map(Math.floor), fr = c.map((v, k) => v - c0[k]);
    let s = 0;
    for (let dz = 0; dz < 2; dz++) for (let dy = 0; dy < 2; dy++) for (let dx = 0; dx < 2; dx++)
      s += A[idx(c0[0] + dx, c0[1] + dy, c0[2] + dz)] * (dx ? fr[0] : 1 - fr[0]) * (dy ? fr[1] : 1 - fr[1]) * (dz ? fr[2] : 1 - fr[2]);
    return s;
  };
  return body.pos.map((p, i) => {
    if (!box.containsPoint(p)) return 0;
    const arm = Math.min(1, Math.max(0, (body.arm[i] - (ARMS - 0.1)) / 0.2));   // (to nothing across the cut)
    const w = read(Wb, p);
    return w > 1e-6 ? read(Sb, p) / w * (1 - arm) : 0;
  });
}

// Holes in it filled: a patch of his surface the first vest didn't cover, but with vest all round it (an oval on each
// side under the arm), is covered. Open stays only what reaches an opening - his arms, his head, his legs, the front
// opening (his chest's middle) - through points the first vest left bare.
function fillHoles(body, cov) {
  const nb = body.pos.map(() => []);
  for (const [a, b, c] of body.tris) { nb[a].push(b, c); nb[b].push(a, c); nb[c].push(a, b); }
  const open = new Uint8Array(body.pos.length), queue = [];
  body.pos.forEach((p, i) => {
    const seed = body.arm[i] > ARMS || p.y > 1.5 || p.y < 0.5 || (Math.abs(p.x) < 0.05 && p.z > 0.15 && p.y > 0.55 && p.y < 1.3);
    if (seed && !cov[i]) { open[i] = 1; queue.push(i); }
  });
  while (queue.length) for (const j of nb[queue.pop()]) if (!open[j] && !cov[j]) { open[j] = 1; queue.push(j); }
  return cov.map((c, i) => c || !open[i] ? 1 : 0);
}

function smooth(body, f, times) {   // (each point the average of it and its neighbours, `times` over)
  const nb = body.pos.map(() => new Set());
  for (const [a, b, c] of body.tris) { nb[a].add(b).add(c); nb[b].add(a).add(c); nb[c].add(a).add(b); }
  for (let it = 0; it < times; it++)
    f = f.map((v, i) => { let s = v; for (const j of nb[i]) s += f[j]; return s / (nb[i].size + 1); });
  return f;
}

// his triangles where `near` is true split in four (the new points on his surface: the edges' middles, their normals
// blended) - the triangles the outline crosses, smaller; elsewhere as they were (cut away, or whole inside the vest)
function subdivide(body, near) {
  const pos = body.pos.slice(), nrm = body.nrm.slice(), tris = [], mids = new Map();
  const mid = (i, j) => {
    const k = i < j ? i + ',' + j : j + ',' + i;
    if (!mids.has(k)) {
      mids.set(k, pos.length);
      pos.push(pos[i].clone().add(pos[j]).multiplyScalar(0.5));
      nrm.push(nrm[i].clone().add(nrm[j]).normalize());
    }
    return mids.get(k);
  };
  body.tris.forEach((t, k) => {
    if (!near[k]) { tris.push(t); return; }
    const [a, b, c] = t, ab = mid(a, b), bc = mid(b, c), ca = mid(c, a);
    tris.push([a, ab, ca], [ab, b, bc], [ca, bc, c], [ab, bc, ca]);
  });
  return {pos, nrm, tris};
}

// the vest's edge loops made smooth: each edge point moved towards the middle of its two neighbours along the edge
// (on his surface, near enough - the moves are small), `times` over
function smoothEdges(s, times) {
  const count = new Map();
  for (const [a, b, c] of s.tris) for (const [x, y] of [[a, b], [b, c], [c, a]]) {
    const k = x < y ? x + ',' + y : y + ',' + x;
    count.set(k, (count.get(k) || 0) + 1);
  }
  const nb = new Map();
  for (const [k, n] of count) {
    if (n !== 1) continue;
    const [x, y] = k.split(',').map(Number);
    if (!nb.has(x)) nb.set(x, []);
    if (!nb.has(y)) nb.set(y, []);
    nb.get(x).push(y); nb.get(y).push(x);
  }
  for (let it = 0; it < times; it++) {
    const next = new Map();
    for (const [i, js] of nb) {
      if (js.length !== 2) continue;   // (where edges meet oddly: left)
      next.set(i, s.pos[i].clone().multiplyScalar(0.5).add(s.pos[js[0]].clone().add(s.pos[js[1]]).multiplyScalar(0.25)));
    }
    for (const [i, p] of next) s.pos[i] = p;
  }
  return s;
}

// his triangles cut along f = 0.5: the part where f >= 0.5 (new points on the edges it crosses, shared by both sides)
function cut(body, f) {
  const pos = [], nrm = [], tris = [], keep = new Map(), edge = new Map();
  const old = i => {
    if (!keep.has(i)) { keep.set(i, pos.length); pos.push(body.pos[i].clone()); nrm.push(body.nrm[i].clone()); }
    return keep.get(i);
  };
  const mid = (i, j) => {
    const k = i < j ? i + ',' + j : j + ',' + i;
    if (!edge.has(k)) {
      const t = (0.5 - f[i]) / (f[j] - f[i]);
      edge.set(k, pos.length);
      pos.push(body.pos[i].clone().lerp(body.pos[j], t));
      nrm.push(body.nrm[i].clone().lerp(body.nrm[j], t).normalize());
    }
    return edge.get(k);
  };
  for (const tri of body.tris) {
    const ins = tri.map(i => f[i] >= 0.5), n = ins.filter(Boolean).length;
    if (!n) continue;
    if (n === 3) { tris.push(tri.map(old)); continue; }
    const r = ins.findIndex((x, k) => x !== ins[(k + 1) % 3] && x !== ins[(k + 2) % 3]);   // (the one on its own)
    const [a, b, c] = [tri[r], tri[(r + 1) % 3], tri[(r + 2) % 3]];
    const ab = mid(a, b), ac = mid(a, c);
    if (n === 1) tris.push([old(a), ab, ac]);
    else { tris.push([ab, old(b), old(c)]); tris.push([ab, old(c), ac]); }
  }
  return {pos, nrm, tris};
}

// lifted off him: an outer surface, an inner one (facing him), and the rim round every edge
function leather(s) {
  const P = [], N = [], I = [];
  const add = (p, n) => { P.push(p.x, p.y, p.z); N.push(n.x, n.y, n.z); return P.length / 3 - 1; };
  const outer = s.pos.map((p, i) => add(p.clone().addScaledVector(s.nrm[i], GAP + THICK), s.nrm[i]));
  const inner = s.pos.map((p, i) => add(p.clone().addScaledVector(s.nrm[i], GAP), s.nrm[i].clone().negate()));
  const count = new Map();
  for (const [a, b, c] of s.tris) {
    I.push(outer[a], outer[b], outer[c], inner[a], inner[c], inner[b]);
    for (const [x, y] of [[a, b], [b, c], [c, a]]) {
      const k = x < y ? x + ',' + y : y + ',' + x;
      const e = count.get(k);
      count.set(k, e ? {n: e.n + 1} : {n: 1, x, y});
    }
  }
  for (const e of count.values()) {   // (an edge of one triangle: the vest's edge - a strip of rim along it)
    if (e.n !== 1) continue;
    const {x, y} = e, out = s.pos[y].clone().sub(s.pos[x]).cross(s.nrm[x].clone().add(s.nrm[y])).normalize();
    const q = [s.pos[x].clone().addScaledVector(s.nrm[x], GAP + THICK), s.pos[y].clone().addScaledVector(s.nrm[y], GAP + THICK),
      s.pos[y].clone().addScaledVector(s.nrm[y], GAP), s.pos[x].clone().addScaledVector(s.nrm[x], GAP)].map(p => add(p, out));
    I.push(q[0], q[3], q[2], q[0], q[2], q[1]);
  }
  const g = new THREE.BufferGeometry();
  g.setAttribute('position', new THREE.Float32BufferAttribute(P, 3));
  g.setAttribute('normal', new THREE.Float32BufferAttribute(N, 3));
  g.setIndex(I);
  return g;
}

// a group of parts moved along his normal so its back sits `on` off him
function seat(G, geos, on) {
  const pts = [];
  for (const g of geos) { const a = g.attributes.position; for (let i = 0; i < a.count; i++) pts.push(new THREE.Vector3().fromBufferAttribute(a, i)); }
  const c = pts.reduce((s, p) => s.add(p), new THREE.Vector3()).multiplyScalar(1 / pts.length);
  const n = outOf(G, c)?.n;
  if (!n) return 0;
  let lo = Infinity;
  for (const p of pts) { const o = outOf(G, p); if (o) lo = Math.min(lo, o.d); }
  const by = on - lo;
  for (const g of geos) { g.translate(n.x * by, n.y * by, n.z * by); }
  return by;
}

export async function build() {
  const ed = await load();
  const src = ed.src.parts, G = bodyGrid(ed.pieces), body = bodyMesh(ed.pieces, ed.skel.names);
  const f = outline(body, fillHoles(body, coverage(body, src[0].geo)));
  const vest = leather(smoothEdges(cut(body, f), 5));
  const geos = src.map((p, k) => k === 0 ? vest : p.geo.clone());
  // the pockets with their flaps and buttons, and the front buttons, back on the new leather
  const groups = [[1, 2, 9], [3, 4, 10], [5, 6, 11], [7, 8, 12], [13], [14], [15], [16], [17], [18]];
  const moved = groups.map(g => seat(G, g.map(k => geos[k]), GAP + THICK + 0.003).toFixed(3));
  const scene = new THREE.Scene();
  src.forEach((p, k) => {
    const color = k === 0 && p.geo.attributes.color ? (() => {   // (the first vest's colour: material x its vertex colours)
      const a = p.geo.attributes.color, c = new THREE.Color(0, 0, 0);
      for (let i = 0; i < a.count; i++) c.r += a.getX(i) / a.count, c.g += a.getY(i) / a.count, c.b += a.getZ(i) / a.count;
      return c.multiply(p.color);
    })() : p.color;
    const mesh = new THREE.Mesh(geos[k], new THREE.MeshStandardMaterial({color, vertexColors: k !== 0 && !!p.geo.attributes.color}));
    mesh.name = 'part' + k;
    scene.add(mesh);
  });
  const glb = await new GLTFExporter().parseAsync(scene, {binary: true});
  await fetch('/save/peter_adventurer_outfit_v2.glb', {method: 'PUT', body: new Blob([glb])});
  return {bodyPoints: body.pos.length, covered: f.filter(v => v >= 0.5).length, vestTriangles: vest.index.count / 3, moved};
}
