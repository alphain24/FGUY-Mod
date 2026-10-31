// The outfit and Peter posed as the game poses them (each vertex moved by its bones: the sum of weight x bone's world
// matrix x its inverse bind), in test poses, and how much of the outfit then lies inside his drawn surface (his
// triangles the outfit hides left out), part by part. Run in the Skin Studio's page after clash_check.js load():
//   (await import('/content/peter-adventurer/pose_check.js')).check()
import * as THREE from 'three';
import {outOf} from './clash_check.js';

const POSES = {   // bone name (start) -> rotation [axis, radians]
  rest: {},
  armsDown: {'Arm L 2 ': ['z', -1.1], 'Arm R 2 ': ['z', 1.1]},
  armsForward: {'Arm L 2 ': ['y', -1.0], 'Arm R 2 ': ['y', 1.0]},
  leanForward: {'Spine 2 ': ['x', 0.35], 'Spine 3 ': ['x', 0.3]},
  legUp: {'Leg L 1 ': ['x', -0.9], 'Leg L 2 ': ['x', 0.9]},
  headTurn: {'Head (': ['y', 0.6], 'Neck ': ['x', 0.25]},
};

function skinMats(skel, pose) {
  const world = [];
  for (let i = 0; i < skel.count; i++) {
    const local = skel.local[i].clone(), name = skel.names[i];
    for (const [start, [axis, a]] of Object.entries(pose)) {
      if (!name.startsWith(start)) continue;
      const r = new THREE.Matrix4()[{x: 'makeRotationX', y: 'makeRotationY', z: 'makeRotationZ'}[axis]](a);
      local.multiply(r);
    }
    world[i] = skel.parent[i] >= 0 ? world[skel.parent[i]].clone().multiply(local) : local;
  }
  return world.map((w, i) => w.clone().multiply(skel.invBind[i]));
}
function skinned(M, p, n, bones, weights) {
  const P = new THREE.Vector3(), N = new THREE.Vector3(), t = new THREE.Vector3();
  let total = 0;
  bones.forEach((b, j) => {
    const w = weights[j];
    if (!(w > 0)) return;
    total += w;
    P.addScaledVector(t.copy(p).applyMatrix4(M[b]), w);
    N.addScaledVector(t.copy(n).transformDirection(M[b]), w);
  });
  return total ? {p: P.multiplyScalar(1 / total), n: N.normalize()} : {p: p.clone(), n: n.clone()};
}

export async function check() {
  const ed = window.studioView.models.state, m = ed.prepared, skel = ed.skel;
  for (let t = 0; t < 40 && !ed.hidden; t++) await new Promise(r => setTimeout(r, 250));
  const hidden = new Map((ed.hidden || []).map(h => [h.piece, new Set(h.ranges.flatMap(([s, n]) => Array.from({length: n}, (_, k) => s + k)))]));
  const partOf = new Int32Array(m.verts.length).fill(-1);
  m.parts.forEach((p, k) => { for (let i = p.first; i < p.first + p.count; i++) partOf[m.idx[i]] = k; });
  const out = {};
  for (const [name, pose] of Object.entries(POSES)) {
    const M = skinMats(skel, pose);
    // his drawn triangles, posed, in a grid (clash_check's layout)
    const cell = 0.05, grid = new Map(), key = (x, y, z) => x + ',' + y + ',' + z;
    for (const p of ed.pieces) {
      if (!p.skin) continue;
      const V = p.position.length / 3, pp = [], nn = [];
      for (let i = 0; i < V; i++) {
        const s = skinned(M, new THREE.Vector3().fromArray(p.position, i * 3), new THREE.Vector3().fromArray(p.normal, i * 3),
          Array.from(p.skin.index.subarray(i * 4, i * 4 + 4)), Array.from(p.skin.weight.subarray(i * 4, i * 4 + 4)));
        pp.push(s.p); nn.push(s.n);
      }
      const gone = hidden.get(p);
      for (let t = 0; t < p.index.length / 3; t++) {
        if (gone?.has(t)) continue;
        const c = [p.index[t * 3], p.index[t * 3 + 1], p.index[t * 3 + 2]];
        const top = (i) => { let b = 0, w = -1; for (let k = 0; k < 4; k++) if (p.skin.weight[i * 4 + k] > w) { w = p.skin.weight[i * 4 + k]; b = p.skin.index[i * 4 + k]; } return b; };
        const tri = {tri: new THREE.Triangle(pp[c[0]], pp[c[1]], pp[c[2]]), n: c.map(i => nn[i]), bone: top(c[0])};
        const box = new THREE.Box3().setFromPoints([tri.tri.a, tri.tri.b, tri.tri.c]);
        for (let x = Math.floor(box.min.x / cell); x <= Math.floor(box.max.x / cell); x++)
          for (let y = Math.floor(box.min.y / cell); y <= Math.floor(box.max.y / cell); y++)
            for (let z = Math.floor(box.min.z / cell); z <= Math.floor(box.max.z / cell); z++) {
              const k = key(x, y, z);
              if (!grid.has(k)) grid.set(k, []);
              grid.get(k).push(tri);
            }
      }
    }
    const G = {grid, key, cell};
    const by = {};
    m.verts.forEach((v, i) => {
      const s = skinned(M, v.pos, v.nrm, v.bw.map(x => x[0]), v.bw.map(x => x[1]));
      const o = outOf(G, s.p, 2);
      if (!o || o.d >= 0) return;
      const k = partOf[i];
      by[k] = by[k] || {n: 0, deepest: 0, in: {}};
      by[k].n++;
      by[k].deepest = Math.min(by[k].deepest, o.d);
      const bn = skel.names[o.bone] || '?';
      by[k].in[bn] = (by[k].in[bn] || 0) + 1;
    });
    out[name] = Object.entries(by).sort((a, b) => a[1].deepest - b[1].deepest)
      .map(([k, s]) => `#${k}: ${s.n} inside, deepest ${s.deepest.toFixed(3)} (in: ${Object.entries(s.in).sort((a, b) => b[1] - a[1]).slice(0, 3).map(([n, c]) => n + ' ' + c).join(', ')})`);
  }
  return out;
}
