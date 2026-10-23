// Models: a new character from a .glb (made in Blender, with img2threejs, ...): fitted onto the character (its size and
// place - automatically, then by hand), its vertices given the character's bones (each takes the weights of the
// character's own vertices nearest it - or, rigged on the character the Studio exports, its own), its colors made a
// texture if it has none, tried in a test pose, saved as a model pack (.fgmodel) for the FGUY Cosmetics mod - the game
// draws it instead of the character (src/loader/models.cpp). mesh.bin as tools/re/glb2fgmodel.py writes it:
//   "FGMD", version 1, bones S, parts P, vertices V, indices I (int32s); parents S int8; inverse binds S x 12 floats
//   (3x4 row-major); parts P x (first index, index count, texture) uint32s; vertices V x (position 3, normal 3, uv 2
//   floats, bones 4 uint8, weights 4 uint8 adding up to 255); indices I x uint16.
import * as THREE from 'three';
import {GLTFLoader} from 'three/addons/loaders/GLTFLoader.js';
import {zipSync, strToU8} from 'fflate';
import {makeRig} from './rig.js';

export function createModelEditor({holder, $, setStatus, download, outfitsOf}) {
  const ed = {outfit: null, pieces: null, skel: null, rig: null, src: null, view: null, posed: false};

  // ---- the character: a ghost to fit to, its vertices' bones to copy ------------------------------------------------
  function build(outfit, pieces, skel) {
    holder.clear();
    ed.outfit = outfit; ed.pieces = pieces; ed.skel = skel;
    ed.rig = makeRig(skel);
    holder.add(ed.rig.root);
    const ghost = new THREE.Group();
    const mat = new THREE.MeshBasicMaterial({color: 0x5cc8ff, wireframe: true, transparent: true, opacity: 0.18});
    for (const p of pieces) {
      if (/damage|dstr/i.test(p.texture?.name || '')) continue;
      const g = new THREE.BufferGeometry();
      g.setAttribute('position', new THREE.BufferAttribute(p.position, 3));
      g.setIndex(new THREE.BufferAttribute(p.index, 1));
      ghost.add(new THREE.Mesh(g, mat));
    }
    ghost.visible = $('modelGhost').checked;
    holder.add(ghost);
    ed.ghost = ghost;
    if (ed.src) refit(true); else drawPreview();
    $('saveModel').disabled = !ed.src;
  }
  $('modelGhost').onchange = () => { if (ed.ghost) ed.ghost.visible = $('modelGhost').checked; };

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
      refit(true);
      $('saveModel').disabled = false;
      setStatus(`Brought in "${f.name}": ${parts.length} part${parts.length > 1 ? 's' : ''}, ${verts.toLocaleString()} vertices` +
        (parts.every(p => p.boneOf) ? ' - rigged on this character: its own weights' : ' - fitted, bones copied from the character') + '.');
    } catch (err) {
      setStatus("Couldn't use that model: " + err.message);
    }
  };

  // ---- fitting: its size and place on the character's ---------------------------------------------------------------
  const box = obj => new THREE.Box3().setFromObject(obj);
  function charBox() {
    const b = new THREE.Box3();
    for (const p of ed.pieces) b.union(new THREE.Box3().setFromArray(p.position));
    return b;
  }
  function srcBox() {
    const b = new THREE.Box3();
    for (const p of ed.src.parts) b.union(new THREE.Box3().setFromBufferAttribute(p.geo.attributes.position));
    return b;
  }
  function refit(auto) {
    if (!ed.src) return;
    if (auto && !ed.src.parts.every(p => p.boneOf)) {   // (as tall as the character, standing where it stands)
      const c = charBox(), s = srcBox(), cs = c.getSize(new THREE.Vector3()), ss = s.getSize(new THREE.Vector3());
      $('fitScale').value = (cs.y / Math.max(ss.y, 1e-6)).toFixed(4);
      $('fitX').value = $('fitY').value = $('fitZ').value = 0;
      $('fitTurn').value = 0;
    } else if (auto) {
      $('fitScale').value = 1; $('fitX').value = $('fitY').value = $('fitZ').value = 0; $('fitTurn').value = 0;
    }
    drawPreview();
  }
  for (const id of ['fitScale', 'fitX', 'fitY', 'fitZ', 'fitTurn']) $(id).oninput = () => drawPreview();
  $('fitAuto').onclick = () => refit(true);

  // a source vertex into the character's space: turned, scaled, its bottom middle on the character's
  function placer() {
    const s = srcBox(), c = charBox();
    const turn = THREE.MathUtils.degToRad(+$('fitTurn').value || 0), scale = +$('fitScale').value || 1;
    const sc = s.getCenter(new THREE.Vector3()), cc = c.getCenter(new THREE.Vector3());
    const rigged = ed.src.parts.every(p => p.boneOf);
    const off = new THREE.Vector3(+$('fitX').value || 0, +$('fitY').value || 0, +$('fitZ').value || 0);
    const q = new THREE.Quaternion().setFromAxisAngle(new THREE.Vector3(0, 1, 0), turn);
    return {
      pos: v => {
        const p = v.clone();
        if (!rigged) p.sub(new THREE.Vector3(sc.x, s.min.y, sc.z));
        p.applyQuaternion(q).multiplyScalar(scale);
        if (!rigged) p.add(new THREE.Vector3(cc.x, c.min.y, cc.z));
        return p.add(off);
      },
      nrm: n => n.clone().applyQuaternion(q),
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
    return {grid, key, cell};
  }
  function weightsAt(G, p) {
    const cx = Math.floor(p.x / G.cell), cy = Math.floor(p.y / G.cell), cz = Math.floor(p.z / G.cell);
    for (let r = 1; r <= 8; r++) {   // (rings out until some are found)
      const near = [];
      for (let x = cx - r; x <= cx + r; x++) for (let y = cy - r; y <= cy + r; y++) for (let z = cz - r; z <= cz + r; z++)
        for (const v of G.grid.get(G.key(x, y, z)) || []) near.push([(v.x - p.x) ** 2 + (v.y - p.y) ** 2 + (v.z - p.z) ** 2, v]);
      if (near.length < 4 && r < 8) continue;
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

  // the model as the game will have it: parts with vertices (character space, bones) and their textures
  function prepare() {
    const P = placer(), rigged = ed.src.parts.every(p => p.boneOf);
    const G = rigged ? null : charGrid(charBox().getSize(new THREE.Vector3()).y / 40);
    const verts = [], idx = [], parts = [], textures = [], palette = [];
    const seenGeo = new Map();   // (a geometry's vertices once, its groups each a part)
    for (const sp of ed.src.parts) {
      const a = sp.geo.attributes;
      if (!seenGeo.has(sp.geo)) {
        const base = verts.length, colors = a.color;
        for (let i = 0; i < a.position.count; i++) {
          const pos = P.pos(new THREE.Vector3().fromBufferAttribute(a.position, i));
          const nrm = P.nrm(new THREE.Vector3().fromBufferAttribute(a.normal, i)).normalize();
          const uv = a.uv ? [a.uv.getX(i), a.uv.getY(i)] : null;
          let bw;
          if (rigged) {
            bw = [];
            for (let j = 0; j < 4; j++) {
              const w = a.skinWeight.getComponent(i, j);
              if (w > 0) bw.push([sp.boneOf[a.skinIndex.getComponent(i, j)], w]);
            }
          } else {
            bw = weightsAt(G, pos);
          }
          const color = colors ? new THREE.Color(colors.getX(i), colors.getY(i), colors.getZ(i)) : null;
          verts.push({pos, nrm, uv, bw, color});
        }
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
        if (tex === 'palette') {   // (a vertex's color: its vertex color times the material's)
          const v = verts[vi];
          if (!v.paletteOf) {
            const c = (v.color || new THREE.Color(1, 1, 1)).clone().multiply(sp.color);
            const key = [c.r, c.g, c.b].map(x => Math.round(x * 63)).join(',');
            let at = palette.indexOf(key);
            if (at < 0) { palette.push(key); at = palette.length - 1; }
            v.paletteOf = at;
          }
        }
      }
      parts.push({first, count: idx.length - first, tex});
    }
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
      bones: S + 1, textures: pngs, tool: 'FGUY Skin Studio'};
    files['model.json'] = [strToU8(JSON.stringify(manifest)), {level: 0}];
    const file = name.replace(/[^\w\- ]+/g, '').trim().replace(/ +/g, '_') + '.fgmodel', blob = new Blob([zipSync(files)]);
    ed.saved = {file, blob};   // (for checking in the browser's console)
    download(blob, file);
    setStatus(`Saved "${name}" for ${o2.character} (${V.toLocaleString()} vertices, ${(I / 3).toLocaleString()} triangles).`);
  };

  return {build, leave() {}, get state() { return ed; }};
}
