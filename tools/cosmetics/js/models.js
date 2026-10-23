// Models: a new character from a .glb (made in Blender, with img2threejs, ...): fitted onto the character (its size and
// place - automatically, then by hand), its vertices given the character's bones (each takes the weights of the
// character's own vertices nearest it - or, rigged on the character the Studio exports, its own), its colors made a
// texture if it has none, tried in a test pose, saved as a model pack (.fgmodel) for the FGUY Cosmetics mod - the game
// draws it instead of the character, or worn over it (outfit pieces: model.json "over": true - src/loader/models.cpp;
// a skin pack can carry one, the Skins tab). One made on this character already (in its space, as the Studio exports
// it) keeps its size and place. mesh.bin as tools/re/glb2fgmodel.py writes it:
//   "FGMD", version 1, bones S, parts P, vertices V, indices I (int32s); parents S int8; inverse binds S x 12 floats
//   (3x4 row-major); parts P x (first index, index count, texture) uint32s; vertices V x (position 3, normal 3, uv 2
//   floats, bones 4 uint8, weights 4 uint8 adding up to 255); indices I x uint16.
import * as THREE from 'three';
import {GLTFLoader} from 'three/addons/loaders/GLTFLoader.js';
import {zipSync, unzipSync, strToU8, strFromU8} from 'fflate';
import {makeRig} from './rig.js';

// A model pack (.fgmodel bytes) as meshes in the character's space, as it stands (the Skins tab shows a skin's model)
export async function modelPackGroup(bytes) {
  const files = unzipSync(bytes), info = JSON.parse(strFromU8(files['model.json']));
  const d = new DataView(files['mesh.bin'].buffer, files['mesh.bin'].byteOffset, files['mesh.bin'].byteLength);
  const [S, P, V, I] = [8, 12, 16, 20].map(o => d.getInt32(o, true));
  let o = 24 + S + S * 48;
  const parts = [];
  for (let k = 0; k < P; k++, o += 12) parts.push([0, 4, 8].map(x => d.getUint32(o + x, true)));
  const pos = new Float32Array(V * 3), nrm = new Float32Array(V * 3), uv = new Float32Array(V * 2);
  for (let i = 0; i < V; i++, o += 40) {
    for (let j = 0; j < 3; j++) { pos[i * 3 + j] = d.getFloat32(o + j * 4, true); nrm[i * 3 + j] = d.getFloat32(o + 12 + j * 4, true); }
    uv[i * 2] = d.getFloat32(o + 24, true); uv[i * 2 + 1] = d.getFloat32(o + 28, true);
  }
  const idx = new Uint16Array(I);
  for (let i = 0; i < I; i++, o += 2) idx[i] = d.getUint16(o, true);
  const texs = [];
  for (const path of info.textures || []) {
    const t = new THREE.CanvasTexture(await createImageBitmap(new Blob([files[path]], {type: 'image/png'})));
    t.flipY = false; t.colorSpace = THREE.SRGBColorSpace;
    texs.push(t);
  }
  const group = new THREE.Group();
  for (const [first, count, tex] of parts) {
    const g = new THREE.BufferGeometry();
    g.setAttribute('position', new THREE.BufferAttribute(pos, 3));
    g.setAttribute('normal', new THREE.BufferAttribute(nrm, 3));
    g.setAttribute('uv', new THREE.BufferAttribute(uv, 2));
    g.setIndex(new THREE.BufferAttribute(idx.slice(first, first + count), 1));
    group.add(new THREE.Mesh(g, new THREE.MeshLambertMaterial({map: texs[tex] || null, side: THREE.DoubleSide, alphaTest: 0.3})));
  }
  return {group, info};
}

export function createModelEditor({holder, $, setStatus, download, outfitsOf, textureFor}) {
  const ed = {outfit: null, pieces: null, skel: null, rig: null, src: null, view: null, posed: false};

  // ---- the character: to fit to, its vertices' bones to copy - shown solid (its own textures), as an x-ray or not
  //      at all, bent by the same bones as the model (a test pose moves both) ------------------------------------------
  async function build(outfit, pieces, skel) {
    holder.clear();
    ed.outfit = outfit; ed.pieces = pieces; ed.skel = skel;
    ed.rig = makeRig(skel);
    holder.add(ed.rig.root);
    const solid = new THREE.Group(), xray = new THREE.Group();
    const wire = new THREE.MeshBasicMaterial({color: 0x5cc8ff, wireframe: true, transparent: true, opacity: 0.18});
    for (const p of pieces) {
      if (/damage|dstr/i.test(p.texture?.name || '')) continue;
      const g = new THREE.BufferGeometry();
      g.setAttribute('position', new THREE.BufferAttribute(p.position, 3));
      if (p.normal) g.setAttribute('normal', new THREE.BufferAttribute(p.normal, 3));
      if (p.uv) g.setAttribute('uv', new THREE.BufferAttribute(p.uv, 2));
      if (p.skin) {
        g.setAttribute('skinIndex', new THREE.Uint16BufferAttribute(p.skin.index, 4));
        g.setAttribute('skinWeight', new THREE.Float32BufferAttribute(p.skin.weight, 4));
      }
      g.setIndex(new THREE.BufferAttribute(p.index, 1));
      const t = p.texture && textureFor ? await textureFor(p.texture) : null;
      const make = mat => {
        const m = p.skin ? new THREE.SkinnedMesh(g, mat) : new THREE.Mesh(g, mat);
        if (p.skin) m.bind(ed.rig.skeleton, new THREE.Matrix4());
        m.frustumCulled = false;
        m.userData.piece = p;   // (its triangles under a model worn over it: hidden - showHidden)
        return m;
      };
      solid.add(make(new THREE.MeshLambertMaterial({map: t ? t.tex : null, side: THREE.DoubleSide, alphaTest: 0.3})));
      xray.add(make(wire));
    }
    if (ed.outfit !== outfit) return;   // (another outfit picked meanwhile)
    holder.add(solid, xray);
    ed.solid = solid; ed.xray = xray;
    showCharacter();
    if (ed.src) refit(true); else drawPreview();
    $('saveModel').disabled = !ed.src;
  }
  function showCharacter() {
    if (!ed.solid) return;
    ed.solid.visible = $('modelGhost').value === 'solid';
    ed.xray.visible = $('modelGhost').value === 'xray';
  }
  $('modelGhost').onchange = showCharacter;

  // ---- the .glb ---------------------------------------------------------------------------------------------------
  $('importModel').onchange = async e => {
    const f = e.target.files[0];
    e.target.value = '';
    if (!f || !ed.skel) return;
    try {
      const g = await new GLTFLoader().parseAsync(await f.arrayBuffer(), '');
      g.scene.updateMatrixWorld(true);
      const parts = [];
      g.scene.traverse(o => {
        if (!o.isMesh || !o.geometry?.attributes.position) return;
        const mats = Array.isArray(o.material) ? o.material : [o.material];
        let geo = o.geometry.clone();
        if (!o.isSkinnedMesh) geo.applyMatrix4(o.matrixWorld);   // (a rigged one: as bound - its bind pose)
        if (!geo.index) geo.setIndex([...Array(geo.attributes.position.count).keys()]);
        if (!geo.attributes.normal) geo.computeVertexNormals();
        const groups = geo.groups.length ? geo.groups : [{start: 0, count: geo.index.count, materialIndex: 0}];
        for (const gr of groups) {
          const m = mats[gr.materialIndex] || mats[0];
          // its own bones, if it's rigged on this character's skeleton (bones named bone00, bone01... - see Emotes)
          let boneOf = null;
          if (o.isSkinnedMesh) boneOf = o.skeleton.bones.map(b => (/^bone(\d+)$/.exec(b.name) || [])[1]).map(x => (x === undefined ? -1 : +x));
          parts.push({geo, start: gr.start, count: gr.count, color: m?.color ? m.color.clone() : new THREE.Color(1, 1, 1),
            map: m?.map?.image || null, boneOf: boneOf && boneOf.every(x => x >= 0) ? boneOf : null});
        }
      });
      if (!parts.length) throw new Error('no meshes in it');
      const verts = parts.reduce((n, p) => n + p.geo.attributes.position.count, 0);
      if (verts > 65535) throw new Error(`it has ${verts.toLocaleString()} vertices - 65,535 at most`);
      ed.src = {parts, name: f.name.replace(/\.(glb|gltf)$/i, '')};
      if (!$('modelName').value) $('modelName').value = ed.src.name;
      // (inside the character's box, about its size or smaller: made on it already - its place kept, and most likely
      //  pieces worn over it; above it, room for a tall hat)
      const s = srcBox(), c = charBox(), pad = c.getSize(new THREE.Vector3()).multiplyScalar(0.15);
      const made = new THREE.Box3(c.min.clone().sub(pad), c.max.clone().add(pad).add(new THREE.Vector3(0, pad.y, 0))).containsBox(s);
      $('fitKeep').checked = made;
      $('modelOver').checked = made;
      refit(true);
      $('saveModel').disabled = false;
      setStatus(`Brought in "${f.name}": ${parts.length} part${parts.length > 1 ? 's' : ''}, ${verts.toLocaleString()} vertices` +
        (parts.every(p => p.boneOf) ? ' - rigged on this character: its own weights' : ' - fitted, bones copied from the character') + '.');
    } catch (err) {
      setStatus("Couldn't use that model: " + err.message);
    }
  };

  // ---- fitting: its size and place on the character's ---------------------------------------------------------------
  // "Fit to the body's shape" (the default): stretched onto the character, part by part - its arms (its widest part,
  // standing in a T-pose) at the character's arm height and span, its torso and legs as wide, deep and long as the
  // character's, its head from the character's shoulders to its top. Off: only as tall as the character. Then the
  // sliders: a size (around its feet), a turn, a move.
  function charBox() {
    const b = new THREE.Box3();
    for (const p of ed.pieces) if (!/damage|dstr/i.test(p.texture?.name || '')) b.union(new THREE.Box3().setFromArray(p.position));
    return b;
  }
  function srcBox() {
    const b = new THREE.Box3();
    for (const p of ed.src.parts) b.union(new THREE.Box3().setFromBufferAttribute(p.geo.attributes.position));
    return b;
  }
  function refit(auto) {
    if (!ed.src) return;
    if (auto) {
      $('fitScale').value = 1;
      $('fitX').value = $('fitY').value = $('fitZ').value = 0;
      $('fitTurn').value = 0;
    }
    drawPreview();
  }
  for (const id of ['fitScale', 'fitX', 'fitY', 'fitZ', 'fitTurn']) $(id).oninput = () => drawPreview();
  $('fitBody').onchange = () => drawPreview();
  $('fitKeep').onchange = () => drawPreview();
  $('modelOver').onchange = () => drawPreview();   // (worn over: each piece rides its own bones)
  $('fitAuto').onclick = () => refit(true);

  // where its arms are, how wide its torso is... - of points (x, y, z: its feet at y 0, its middle at x 0, z 0)
  function shapeOf(pts, armY0) {
    let top = 0, span = 0;
    for (const p of pts) { top = Math.max(top, p.y); span = Math.max(span, Math.abs(p.x)); }
    let armY = armY0, th = 0;
    if (armY === undefined) {   // (its arms: the points out at its sides)
      let sum = 0, n = 0;
      for (const p of pts) if (Math.abs(p.x) > span * 0.8) { sum += p.y; n++; }
      armY = n ? sum / n : top * 0.75;
    }
    for (const p of pts) if (Math.abs(p.x) > span * 0.8) th = Math.max(th, Math.abs(p.y - armY));
    let torso = 0, depth = 0;   // (its torso: the band below its arms, above its feet)
    for (const p of pts) {
      if (p.y < armY * 0.4 || p.y > armY - Math.max(th, armY * 0.08) * 1.2) continue;
      torso = Math.max(torso, Math.abs(p.x));
      depth = Math.max(depth, Math.abs(p.z));
    }
    if (!torso) torso = span * 0.4;
    if (!depth) depth = torso;
    let legs = 0, legDepth = 0;   // (its legs: the lowest band)
    for (const p of pts) {
      if (p.y > armY * 0.3) continue;
      legs = Math.max(legs, Math.abs(p.x));
      legDepth = Math.max(legDepth, Math.abs(p.z));
    }
    return {top, span, armY, th: Math.max(th, top * 0.03), torso, depth, legs: legs || torso, legDepth: legDepth || depth,
      arms: span > torso * 1.3};
  }

  // a source vertex (and its normal) into the character's space
  function placer() {
    const s = srcBox(), c = charBox();
    const turn = THREE.MathUtils.degToRad(+$('fitTurn').value || 0), size = +$('fitScale').value || 1;
    const sc = s.getCenter(new THREE.Vector3()), cc = c.getCenter(new THREE.Vector3());
    const rigged = ed.src.parts.every(p => p.boneOf);
    const off = new THREE.Vector3(+$('fitX').value || 0, +$('fitY').value || 0, +$('fitZ').value || 0);
    const q = new THREE.Quaternion().setFromAxisAngle(new THREE.Vector3(0, 1, 0), turn);
    const local = v => v.clone().sub(new THREE.Vector3(sc.x, s.min.y, sc.z)).applyQuaternion(q);
    if (rigged || $('fitKeep').checked) return {   // (made on this character: as it is)
      map: (v, n) => ({pos: v.clone().applyQuaternion(q).multiplyScalar(size).add(off), nrm: n.clone().applyQuaternion(q)}),
    };
    // the character's shape: its own points, its arm height from its skeleton
    const cpts = [];
    for (const p of ed.pieces) {
      if (/damage|dstr/i.test(p.texture?.name || '')) continue;
      for (let i = 0; i < p.position.length; i += 3) cpts.push({x: p.position[i] - cc.x, y: p.position[i + 1] - c.min.y, z: p.position[i + 2] - cc.z});
    }
    const arm = ed.skel.names.findIndex(nm => /^Arm L 2 /.test(nm));
    const C = shapeOf(cpts, arm >= 0 ? new THREE.Vector3().setFromMatrixPosition(ed.skel.bind[arm]).y - c.min.y : undefined);
    const mpts = [];
    for (const sp of ed.src.parts) {
      const a = sp.geo.attributes.position;
      for (let i = 0; i < a.count; i++) mpts.push(local(new THREE.Vector3().fromBufferAttribute(a, i)));
    }
    const M = shapeOf(mpts);
    const body = $('fitBody').checked;
    const k = C.top / Math.max(M.top, 1e-6);   // (only as tall)
    const yOf = y => {   // (its feet to its arms onto the character's, its arms to its top onto the character's)
      if (!body || !M.arms) return y * k;
      return y <= M.armY ? y / M.armY * C.armY : C.armY + (y - M.armY) / Math.max(M.top - M.armY, 1e-6) * (C.top - C.armY);
    };
    const headK = M.arms ? (C.top - C.armY) / Math.max(M.top - M.armY, 1e-6) : k;
    const inArm = p => Math.abs(p.y - M.armY) <= M.th * 1.5 && Math.abs(p.x) > M.torso;
    const factors = p => {   // (how it's stretched there: x, y, z)
      if (!body || !M.arms) return [k, k, k];
      const ky = p.y <= M.armY ? C.armY / M.armY : headK;
      if (inArm(p)) return [(C.span - C.torso) / Math.max(M.span - M.torso, 1e-6), ky, C.depth / M.depth];
      if (p.y > M.armY + M.th) return [headK, ky, headK];   // (its head: its own shape, as big as the character's)
      const legsK = Math.min(1, Math.max(0, (M.armY * 0.45 - p.y) / (M.armY * 0.15)));   // (its legs, low down - its torso above)
      const t = 1 - legsK;
      return [t * C.torso / M.torso + legsK * C.legs / M.legs, ky, t * C.depth / M.depth + legsK * C.legDepth / M.legDepth];
    };
    const xOf = (p, f) => {
      if (body && M.arms && inArm(p)) return Math.sign(p.x) * (C.torso + (Math.abs(p.x) - M.torso) * f[0]);   // (an arm: to the hand)
      return p.x * f[0];
    };
    return {
      map: (v, n) => {
        const p = local(v), f = factors(p);
        const pos = new THREE.Vector3(xOf(p, f), yOf(p.y), p.z * f[2]).multiplyScalar(size)
          .add(new THREE.Vector3(cc.x, c.min.y, cc.z)).add(off);
        const nn = n.clone().applyQuaternion(q);
        return {pos, nrm: new THREE.Vector3(nn.x / f[0], nn.y / f[1], nn.z / f[2]).normalize()};
      },
    };
  }

  // ---- its bones: the nearest of the character's vertices' (a grid of them, for speed) ------------------------------
  function charGrid(cell) {
    const grid = new Map(), key = (x, y, z) => x + ',' + y + ',' + z;
    for (const p of ed.pieces) {
      if (!p.skin || /damage|dstr/i.test(p.texture?.name || '')) continue;
      for (let i = 0; i < p.position.length / 3; i++) {
        const x = p.position[i * 3], y = p.position[i * 3 + 1], z = p.position[i * 3 + 2];
        const k = key(Math.floor(x / cell), Math.floor(y / cell), Math.floor(z / cell));
        if (!grid.has(k)) grid.set(k, []);
        grid.get(k).push({x, y, z, b: p.skin.index.subarray(i * 4, i * 4 + 4), w: p.skin.weight.subarray(i * 4, i * 4 + 4)});
      }
    }
    return {grid, key, cell, cx: charBox().getCenter(new THREE.Vector3()).x};
  }
  // (side: a part all on the character's left (1) or right (-1) takes only that side's vertices - a boot's inside next
  //  to the other leg would otherwise follow that leg, and the feet stick together)
  // (n, the model's normal there - given, the character's vertices "under" it count: one straight beneath it along n is
  //  nearer than one as far off to the side or outside it - a boot's bulging front takes the shin's weights, not the
  //  shoe's just below it; a vest's hem its own belly's, not the thighs')
  function weightsAt(G, p, side = 0, n = null) {
    const cx = Math.floor(p.x / G.cell), cy = Math.floor(p.y / G.cell), cz = Math.floor(p.z / G.cell);
    const dist = v => {
      const dx = v.x - p.x, dy = v.y - p.y, dz = v.z - p.z;
      if (!n) return dx * dx + dy * dy + dz * dz;
      const along = dx * n.x + dy * n.y + dz * n.z, side2 = dx * dx + dy * dy + dz * dz - along * along;
      return side2 * 4 + along * along * (along > 0 ? 4 : 1);
    };
    for (let r = n ? 2 : 1; r <= 8; r++) {   // (rings out until some are found)
      const near = [];
      for (let x = cx - r; x <= cx + r; x++) for (let y = cy - r; y <= cy + r; y++) for (let z = cz - r; z <= cz + r; z++)
        for (const v of G.grid.get(G.key(x, y, z)) || [])
          if (!side || (v.x - G.cx) * side >= 0) near.push([dist(v), v]);
      if (near.length < (n ? 6 : 4) && r < 8) continue;
      near.sort((a, b) => a[0] - b[0]);
      const acc = new Map();
      for (const [d, v] of near.slice(0, 6)) {
        const k = 1 / (d + 1e-6);
        for (let j = 0; j < 4; j++) if (v.w[j] > 0) acc.set(v.b[j], (acc.get(v.b[j]) || 0) + v.w[j] * k);
      }
      return [...acc.entries()].sort((a, b) => b[1] - a[1]).slice(0, 4);
    }
    return [[0, 1]];
  }

  // Worn over the character, how each piece moves: one sitting on a bigger piece of the model (a lace or an eyelet on
  // a boot, a button on a pocket, the band on the hat - most of its vertices within RIDE of that piece's) takes that
  // piece's weights, vertex by vertex - it moves exactly with it, never sinking into it as it bends; one on its own that
  // mostly follows one bone (RIGID of its weight or more: the hat on the head, the sheath on the hips) follows that bone
  // alone - not the eyes beside a hat's brim; the rest keep the character's own weights (a boot: the leg's, knee and
  // all - the leg inside never comes through it), bar the faintest (under 2%).
  function ride(verts, pieces) {
    const RIDE = 0.08, RIGID = 0.7;
    const boxOf = ([a, b]) => { const box = new THREE.Box3(); for (let i = a; i < b; i++) box.expandByPoint(verts[i].pos); return box; };
    const boxes = pieces.map(boxOf), size = pieces.map(([a, b]) => b - a);
    const order = pieces.map((_, k) => k).sort((x, y) => size[y] - size[x]);   // (biggest first: hosts before riders)
    const done = new Set();
    for (const k of order) {
      const [a, b] = pieces[k];
      let host = -1, best = 0;
      for (const h of order) {   // (a bigger piece, already settled, most of this one's vertices near it)
        if (h === k || !done.has(h) || size[h] < size[k] * 2) continue;
        if (!boxes[h].clone().expandByScalar(RIDE).intersectsBox(boxes[k])) continue;
        let near = 0;
        for (let i = a; i < b; i++) {
          for (let j = pieces[h][0]; j < pieces[h][1]; j++) if (verts[i].pos.distanceToSquared(verts[j].pos) < RIDE * RIDE) { near++; break; }
        }
        if (near > best) { best = near; host = h; }
      }
      if (host >= 0 && best >= (b - a) * 0.5) {
        for (let i = a; i < b; i++) {   // (each vertex: its nearest host vertex's weights)
          let nearest = -1, d = Infinity;
          for (let j = pieces[host][0]; j < pieces[host][1]; j++) {
            const e = verts[i].pos.distanceToSquared(verts[j].pos);
            if (e < d) { d = e; nearest = j; }
          }
          verts[i].bw = verts[nearest].bw.map(x => x.slice());
        }
      } else {
        const acc = new Map();
        for (let i = a; i < b; i++) for (const [bone, w] of verts[i].bw) acc.set(bone, (acc.get(bone) || 0) + w);
        const total = [...acc.values()].reduce((x, w) => x + w, 0), top = [...acc.entries()].sort((x, y) => y[1] - x[1]);
        for (let i = a; i < b; i++) {
          if (top.length && top[0][1] >= total * RIGID) { verts[i].bw = [[top[0][0], 1]]; continue; }
          const bw = verts[i].bw.filter(([bone]) => acc.get(bone) >= total * 0.02);
          if (bw.length) verts[i].bw = bw;
        }
      }
      done.add(k);
    }
  }

  // the model as the game will have it: parts with vertices (character space, bones) and their textures
  function prepare() {
    const P = placer(), rigged = ed.src.parts.every(p => p.boneOf);
    const G = rigged ? null : charGrid(charBox().getSize(new THREE.Vector3()).y / 40);
    const verts = [], idx = [], parts = [], textures = [], palette = [];
    const seenGeo = new Map();   // (a geometry's vertices once, its groups each a part)
    const pieces = [];           // (each geometry's vertices: [from, to])
    for (const sp of ed.src.parts) {
      const a = sp.geo.attributes;
      if (!seenGeo.has(sp.geo)) {
        const base = verts.length, colors = a.color;
        const mapped = [];
        for (let i = 0; i < a.position.count; i++)
          mapped.push(P.map(new THREE.Vector3().fromBufferAttribute(a.position, i), new THREE.Vector3().fromBufferAttribute(a.normal, i)));
        let side = 0;   // (all - bar 2% - on one side of the character's middle: that side's bones only)
        if (G) {
          const left = mapped.filter(m => m.pos.x > G.cx).length;
          if (left >= mapped.length * 0.98) side = 1;
          else if (left <= mapped.length * 0.02) side = -1;
        }
        for (let i = 0; i < a.position.count; i++) {
          const {pos, nrm} = mapped[i];
          const uv = a.uv ? [a.uv.getX(i), a.uv.getY(i)] : null;
          let bw;
          if (rigged) {
            bw = [];
            for (let j = 0; j < 4; j++) {
              const w = a.skinWeight.getComponent(i, j);
              if (w > 0) bw.push([sp.boneOf[a.skinIndex.getComponent(i, j)], w]);
            }
          } else {
            bw = weightsAt(G, pos, side, $('modelOver').checked ? nrm : null);
          }
          const color = colors ? new THREE.Color(colors.getX(i), colors.getY(i), colors.getZ(i)) : null;
          verts.push({pos, nrm, uv, bw, color});
        }
        pieces.push([base, verts.length]);
        seenGeo.set(sp.geo, base);
      }
      const base = seenGeo.get(sp.geo);
      let tex;
      if (sp.map) {   // (its own picture)
        tex = textures.indexOf(sp.map);
        if (tex < 0) { textures.push(sp.map); tex = textures.length - 1; }
      } else {        // (no picture: its colors on the palette - texture "palette")
        tex = 'palette';
      }
      const first = idx.length;
      for (let k = sp.start; k < sp.start + sp.count; k++) {
        const vi = base + sp.geo.index.getX(k);
        idx.push(vi);
        if (tex === 'palette') {   // (a vertex's color: its vertex color times the material's - both linear, as
          const v = verts[vi];     //  three.js keeps them; the palette is a picture: sRGB)
          if (!v.paletteOf) {
            const c = (v.color || new THREE.Color(1, 1, 1)).clone().multiply(sp.color).convertLinearToSRGB();
            const key = [c.r, c.g, c.b].map(x => Math.round(x * 63)).join(',');
            let at = palette.indexOf(key);
            if (at < 0) { palette.push(key); at = palette.length - 1; }
            v.paletteOf = at;
          }
        }
      }
      parts.push({first, count: idx.length - first, tex});
    }
    if (G && $('modelOver').checked) ride(verts, pieces);
    // (the palette: a color an 8 x 8 cell, 32 a row - the game's 4 x 4 compressed blocks and smaller mip levels keep it)
    if (palette.length > 1024) throw new Error('too many colors (' + palette.length + ') - 1,024 at most without a texture');
    if (palette.length) {
      textures.push('palette');
      for (const v of verts) if (v.paletteOf !== undefined) v.uv = [((v.paletteOf % 32) + 0.5) / 32, (Math.floor(v.paletteOf / 32) + 0.5) / 32];
    }
    for (const p of parts) p.texture = p.tex === 'palette' ? textures.length - 1 : p.tex;
    for (const v of verts) v.uv = v.uv || [0, 0];
    return {verts, idx, parts, textures, palette};
  }

  // a texture as a canvas (a picture: up to 1024, a power of two; the palette: 256 x 256)
  function textureCanvas(t, palette) {
    const c = document.createElement('canvas');
    if (t === 'palette') {
      c.width = c.height = 256;
      const x = c.getContext('2d');
      palette.forEach((key, i) => {
        const [r, g, b] = key.split(',').map(Number);
        x.fillStyle = `rgb(${Math.round(r * 255 / 63)},${Math.round(g * 255 / 63)},${Math.round(b * 255 / 63)})`;
        x.fillRect((i % 32) * 8, Math.floor(i / 32) * 8, 8, 8);
      });
      return c;
    }
    const p2 = n => Math.min(1024, 2 ** Math.round(Math.log2(Math.max(4, n))));
    c.width = p2(t.width); c.height = p2(t.height);
    c.getContext('2d').drawImage(t, 0, 0, c.width, c.height);
    return c;
  }

  // ---- the preview: skinned on the character's bones ----------------------------------------------------------------
  function drawPreview() {
    if (ed.view) { holder.remove(ed.view); ed.view = null; }
    if (!ed.src || !ed.rig) return;
    let m;
    try { m = prepare(); } catch (err) { setStatus(err.message); return; }
    ed.prepared = m;
    const view = new THREE.Group();
    const texs = m.textures.map(t => { const tx = new THREE.CanvasTexture(textureCanvas(t, m.palette)); tx.flipY = false; tx.colorSpace = THREE.SRGBColorSpace; return tx; });
    const n = m.verts.length, pos = new Float32Array(n * 3), nrm = new Float32Array(n * 3), uv = new Float32Array(n * 2);
    const si = new Uint16Array(n * 4), sw = new Float32Array(n * 4);
    m.verts.forEach((v, i) => {
      pos.set([v.pos.x, v.pos.y, v.pos.z], i * 3); nrm.set([v.nrm.x, v.nrm.y, v.nrm.z], i * 3); uv.set(v.uv, i * 2);
      const total = v.bw.reduce((s, x) => s + x[1], 0) || 1;
      v.bw.forEach(([b, w], j) => { si[i * 4 + j] = b; sw[i * 4 + j] = w / total; });
    });
    for (const p of m.parts) {
      const g = new THREE.BufferGeometry();
      g.setAttribute('position', new THREE.BufferAttribute(pos, 3));
      g.setAttribute('normal', new THREE.BufferAttribute(nrm, 3));
      g.setAttribute('uv', new THREE.BufferAttribute(uv, 2));
      g.setAttribute('skinIndex', new THREE.Uint16BufferAttribute(si, 4));
      g.setAttribute('skinWeight', new THREE.Float32BufferAttribute(sw, 4));
      g.setIndex(m.idx.slice(p.first, p.first + p.count));
      const mesh = new THREE.SkinnedMesh(g, new THREE.MeshLambertMaterial({map: texs[p.texture], side: THREE.DoubleSide, alphaTest: 0.3}));
      mesh.bind(ed.rig.skeleton, new THREE.Matrix4());
      mesh.frustumCulled = false;
      view.add(mesh);
    }
    holder.add(view);
    ed.view = view;
    clearTimeout(ed.hideTimer);   // (the character's covered triangles - after a moment: a slider drag doesn't wait)
    ed.hideTimer = setTimeout(() => { ed.hidden = $('modelOver').checked ? hiddenUnder(m) : []; showHidden(); }, 300);
  }

  // ---- the character's own triangles under a model worn over it: not drawn (model.json "hide" - the game leaves them
  //      out of its draws, models.cpp). Where the model is solid round the body - a boot round the shin and shoe, a
  //      hat's crown round the top of the head - the body can't show through it as both move. A triangle is under it
  //      when two of its corners are inside one of the model's solid parts: rays out of the point (three ways, both
  //      directions) cross that part an odd number of times, none nearer than MARGIN, and it is THICK or more
  //      through the point every way - a vest's leather or an edge trim, thin one way, hides nothing.
  function hiddenUnder(m) {
    const parts = m.parts.map(p => {
      const t = new Float32Array(p.count * 3), box = new THREE.Box3();
      for (let k = 0; k < p.count; k++) {
        const v = m.verts[m.idx[p.first + k]].pos;
        t.set([v.x, v.y, v.z], k * 3);
        box.expandByPoint(v);
      }
      return {t, box};
    });
    const MARGIN = 0.004, THICK = 0.12;
    const crossings = (t, o, d) => {   // (Moller-Trumbore, both faces: how many, and the nearest)
      let n = 0, near = Infinity;
      for (let i = 0; i < t.length; i += 9) {
        const e1x = t[i + 3] - t[i], e1y = t[i + 4] - t[i + 1], e1z = t[i + 5] - t[i + 2];
        const e2x = t[i + 6] - t[i], e2y = t[i + 7] - t[i + 1], e2z = t[i + 8] - t[i + 2];
        const px = d[1] * e2z - d[2] * e2y, py = d[2] * e2x - d[0] * e2z, pz = d[0] * e2y - d[1] * e2x;
        const det = e1x * px + e1y * py + e1z * pz;
        if (Math.abs(det) < 1e-12) continue;
        const sx = o[0] - t[i], sy = o[1] - t[i + 1], sz = o[2] - t[i + 2];
        const u = (sx * px + sy * py + sz * pz) / det;
        if (u < 0 || u > 1) continue;
        const qx = sy * e1z - sz * e1y, qy = sz * e1x - sx * e1z, qz = sx * e1y - sy * e1x;
        const v = (d[0] * qx + d[1] * qy + d[2] * qz) / det;
        if (v < 0 || u + v > 1) continue;
        const dist = (e2x * qx + e2y * qy + e2z * qz) / det;
        if (dist > 1e-6) { n++; near = Math.min(near, dist); }
      }
      return {n, near};
    };
    const deepIn = (q, o) => {
      for (const d of [D1, D2, D3]) {
        const a = crossings(q.t, o, d), b = crossings(q.t, o, d.map(x => -x));
        if (a.n % 2 !== 1 || b.n % 2 !== 1 || Math.min(a.near, b.near) < MARGIN || a.near + b.near < THICK) return false;
      }
      return true;
    };
    const D1 = [1, 0.0131, 0.0071], D2 = [0.0113, 1, 0.0171], D3 = [0.0093, 0.0127, 1];   // (a little off the axes)
    const out = [], pt = new THREE.Vector3();
    for (const p of ed.pieces) {
      const V = p.position.length / 3, inside = new Uint8Array(V);
      for (let i = 0; i < V; i++) {
        pt.fromArray(p.position, i * 3);
        const o = [pt.x, pt.y, pt.z];
        inside[i] = parts.some(q => q.box.containsPoint(pt) && deepIn(q, o)) ? 1 : 0;
      }
      const ranges = [];
      let hidden = 0;
      for (let k = 0; k < p.index.length / 3; k++) {
        if (inside[p.index[k * 3]] + inside[p.index[k * 3 + 1]] + inside[p.index[k * 3 + 2]] < 2) continue;
        hidden++;
        const last = ranges[ranges.length - 1];
        if (last && last[0] + last[1] === k) last[1]++; else ranges.push([k, 1]);
      }
      if (hidden) out.push({piece: p, verts: V, indices: p.index.length, ranges, hidden});
    }
    return out;
  }
  function showHidden() {   // (the preview: the character with those triangles left out)
    for (const g of [ed.solid, ed.xray]) for (const mesh of g?.children || []) {
      const p = mesh.userData.piece, h = (ed.hidden || []).find(x => x.piece === p);
      let index = p.index;
      if (h) {
        index = p.index.slice();
        for (const [s, n] of h.ranges) for (let k = s; k < s + n; k++) index[k * 3 + 1] = index[k * 3 + 2] = index[k * 3];
      }
      mesh.geometry.setIndex(new THREE.BufferAttribute(index, 1));
    }
  }

  // a test pose: arms down and out, the head turned - how its bones bend it
  $('modelPose').onclick = () => {
    if (!ed.rig) return;
    ed.posed = !ed.posed;
    const names = ed.skel.names;
    ed.rig.bones.forEach((b, i) => { ed.skel.local[i].decompose(b.position, b.quaternion, b.scale); });
    if (ed.posed) ed.rig.bones.forEach((b, i) => {
      if (/^Arm [LR] 2 /.test(names[i])) b.rotateZ(/ L /.test(names[i]) ? -0.9 : 0.9);
      if (/^Head /.test(names[i])) b.rotateY(0.5);
      if (/^Leg L 1 /.test(names[i])) b.rotateZ(0.4);
    });
    $('modelPose').textContent = ed.posed ? 'Back to the rest pose' : 'Try a pose';
  };

  // ---- the pack -------------------------------------------------------------------------------------------------------
  $('saveModel').onclick = async () => {
    if (!ed.src) return;
    let m;
    try { m = prepare(); } catch (err) { setStatus(err.message); return; }
    const S = ed.skel.count, V = m.verts.length, I = m.idx.length, P = m.parts.length;
    const buf = new ArrayBuffer(24 + S + S * 48 + P * 12 + V * 40 + I * 2), dv = new DataView(buf);
    let o = 0;
    const i32 = v => { dv.setInt32(o, v, true); o += 4; }, f32 = v => { dv.setFloat32(o, v, true); o += 4; };
    dv.setUint32(0, 0x444d4746, true); o = 4;   // "FGMD"
    [1, S, P, V, I].forEach(i32);
    ed.skel.parent.forEach(p => { dv.setInt8(o, p); o += 1; });
    for (const m4 of ed.skel.invBind) { const e = m4.elements; [0, 4, 8, 12, 1, 5, 9, 13, 2, 6, 10, 14].forEach(k => f32(e[k])); }
    for (const p of m.parts) { [p.first, p.count, p.texture].forEach(v => { dv.setUint32(o, v, true); o += 4; }); }
    for (const v of m.verts) {
      [v.pos.x, v.pos.y, v.pos.z, v.nrm.x, v.nrm.y, v.nrm.z, v.uv[0], v.uv[1]].forEach(f32);
      const bw = (v.bw.length ? v.bw : [[0, 1]]).slice(0, 4), total = bw.reduce((s, x) => s + x[1], 0) || 1;
      const w = bw.map(x => Math.round(x[1] / total * 255));
      w[w.indexOf(Math.max(...w))] += 255 - w.reduce((s, x) => s + x, 0);
      for (let j = 0; j < 4; j++) dv.setUint8(o + j, bw[j] ? bw[j][0] : 0);
      for (let j = 0; j < 4; j++) dv.setUint8(o + 4 + j, bw[j] ? w[j] : 0);
      o += 8;
    }
    for (const i of m.idx) { dv.setUint16(o, i, true); o += 2; }
    const files = {'mesh.bin': [new Uint8Array(buf), {level: 0}]}, pngs = [];
    for (let k = 0; k < m.textures.length; k++) {
      const c = textureCanvas(m.textures[k], m.palette);
      const png = new Uint8Array(await (await new Promise(r => c.toBlob(r, 'image/png'))).arrayBuffer());
      files['textures/' + k + '.png'] = [png, {level: 0}];
      pngs.push('textures/' + k + '.png');
    }
    const o2 = ed.outfit, name = ($('modelName').value || 'My model').trim();
    const manifest = {format: 1, name, character: o2.character, models: outfitsOf(o2.character).map(x => [x.model.lo, x.model.hi]),
      bones: S + 1, textures: pngs, over: $('modelOver').checked, tool: 'FGUY Skin Studio'};
    if (manifest.over)   // (the character's triangles under it, by piece - its vertex and index counts - and number)
      manifest.hide = hiddenUnder(m).map(h => ({verts: h.verts, indices: h.indices, tris: h.ranges.flat()}));
    files['model.json'] = [strToU8(JSON.stringify(manifest)), {level: 0}];
    const file = name.replace(/[^\w\- ]+/g, '').trim().replace(/ +/g, '_') + '.fgmodel', blob = new Blob([zipSync(files)]);
    ed.saved = {file, blob};   // (for checking in the browser's console)
    download(blob, file);
    setStatus(`Saved "${name}" for ${o2.character} (${V.toLocaleString()} vertices, ${(I / 3).toLocaleString()} triangles).`);
  };

  return {build, leave() {}, get state() { return ed; }};
}
