// Where the outfit and Peter clash, measured in the Skin Studio's page (index.html?dev, tools/cosmetics/dev_server.py):
//   (await import('/content/peter-adventurer/clash_check.js')).check()
// The outfit GLB goes into the Models tab (as studio_build.js does it), then for every vertex of the model as the game
// will have it: the nearest point of Peter's surface and how far out of it the vertex is (along his normal there -
// negative: inside him, his shirt showing through). Per part of the outfit: how many are inside, how many closer than
// `margin`, the deepest.
import * as THREE from 'three';
const sleep = ms => new Promise(r => setTimeout(r, ms));
const $ = id => document.getElementById(id);
async function waitFor(test, ms = 20000) {
  for (let t = 0; t < ms; t += 250) { if (test()) return true; await sleep(250); }
  return false;
}

export async function load(file = 'peter_adventurer_outfit.glb') {
  const studio = window.studio, models = window.studioView.models;
  const peter = () => studio.outfits.find(o => o.model.name === 'Peter_Rig_Geo');
  await waitFor(() => studio.outfits?.length && peter());
  $('tabModels').click();
  peter().chip.click();
  await waitFor(() => models.state.outfit === peter() && models.state.solid);
  const glb = await (await fetch('/research/' + file, {cache: 'no-store'})).blob();
  const dt = new DataTransfer();
  dt.items.add(new File([glb], 'Peter Adventurer.glb'));
  $('importModel').files = dt.files;
  $('importModel').dispatchEvent(new Event('change'));
  await waitFor(() => models.state.src);
  for (const id of ['fitKeep', 'modelOver']) if (!$(id).checked) { $(id).checked = true; $(id).dispatchEvent(new Event('change')); }
  await waitFor(() => models.state.prepared);
  return models.state;
}

// Peter's triangles in a grid (by cell), each with its corners and its vertex normals
export function bodyGrid(pieces, cell = 0.05) {
  const grid = new Map(), key = (x, y, z) => x + ',' + y + ',' + z, tris = [];
  for (const p of pieces) {
    const P = p.position, N = p.normal, I = p.index;
    for (let t = 0; t < I.length; t += 3) {
      const v = [I[t], I[t + 1], I[t + 2]].map(i => new THREE.Vector3(P[i * 3], P[i * 3 + 1], P[i * 3 + 2]));
      const n = [I[t], I[t + 1], I[t + 2]].map(i => new THREE.Vector3(N[i * 3], N[i * 3 + 1], N[i * 3 + 2]));
      const tri = {tri: new THREE.Triangle(...v), n, piece: p};
      tris.push(tri);
      const box = new THREE.Box3().setFromPoints(v);
      for (let x = Math.floor(box.min.x / cell); x <= Math.floor(box.max.x / cell); x++)
        for (let y = Math.floor(box.min.y / cell); y <= Math.floor(box.max.y / cell); y++)
          for (let z = Math.floor(box.min.z / cell); z <= Math.floor(box.max.z / cell); z++) {
            const k = key(x, y, z);
            if (!grid.has(k)) grid.set(k, []);
            grid.get(k).push(tri);
          }
    }
  }
  return {grid, key, cell, tris};
}

// how far out of Peter a point is: the nearest point of his surface, his normal there (the corners' normals blended)
const q = new THREE.Vector3(), bary = new THREE.Vector3();
export function outOf(G, p, reach = 3) {
  const cx = Math.floor(p.x / G.cell), cy = Math.floor(p.y / G.cell), cz = Math.floor(p.z / G.cell);
  let best = null, bd = Infinity;
  for (let r = 1; r <= reach && !best; r++)
    for (let x = cx - r; x <= cx + r; x++) for (let y = cy - r; y <= cy + r; y++) for (let z = cz - r; z <= cz + r; z++)
      for (const t of G.grid.get(G.key(x, y, z)) || []) {
        t.tri.closestPointToPoint(p, q);
        const d = q.distanceToSquared(p);
        if (d < bd) { bd = d; best = {t, at: q.clone()}; }
      }
  if (!best) return null;
  best.t.tri.getBarycoord(best.at, bary);
  const n = best.t.n[0].clone().multiplyScalar(bary.x).add(best.t.n[1].clone().multiplyScalar(bary.y))
    .add(best.t.n[2].clone().multiplyScalar(bary.z)).normalize();
  return {d: p.clone().sub(best.at).dot(n), at: best.at, n, bone: best.t.bone};
}

export async function check(margin = 0.008, file) {
  const ed = await load(file);
  const m = ed.prepared, G = bodyGrid(ed.pieces);
  const names = ed.src.parts.map((p, k) => {   // (the GLB's parts have no names here: colour, size, where)
    const box = new THREE.Box3();
    for (let i = m.parts[k].first; i < m.parts[k].first + m.parts[k].count; i++) box.expandByPoint(m.verts[m.idx[i]].pos);
    const c = box.getCenter(new THREE.Vector3());
    return `#${k} ${p.color?.getHexString?.() ?? ''} at (${c.x.toFixed(2)}, ${c.y.toFixed(2)}, ${c.z.toFixed(2)})`;
  });
  const partOf = new Int32Array(m.verts.length).fill(-1);
  m.parts.forEach((p, k) => { for (let i = p.first; i < p.first + p.count; i++) partOf[m.idx[i]] = k; });
  const by = new Map();
  m.verts.forEach((v, i) => {
    const o = outOf(G, v.pos);
    if (!o) return;
    const name = names[partOf[i]] ?? '?';
    const s = by.get(name) || {verts: 0, inside: 0, close: 0, deepest: 0};
    s.verts++;
    if (o.d < 0) s.inside++;
    if (o.d < margin) s.close++;
    s.deepest = Math.min(s.deepest, o.d);
    by.set(name, s);
  });
  return [...by.entries()].filter(([, s]) => s.close).sort((a, b) => a[1].deepest - b[1].deepest)
    .map(([name, s]) => `${name}: ${s.inside} of ${s.verts} inside, ${s.close} closer than ${margin}, deepest ${s.deepest.toFixed(4)}`);
}
