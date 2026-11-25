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
import {toPng, NEEDS_TURNING} from './pictures.js';
import {FBXLoader} from 'three/addons/loaders/FBXLoader.js';
import {OBJLoader} from 'three/addons/loaders/OBJLoader.js';
import {MTLLoader} from 'three/addons/loaders/MTLLoader.js';
import {zipSync, unzipSync, strToU8, strFromU8} from 'fflate';
import {makeRig} from './rig.js';
import {LANDMARKS, SEGMENTS, guessJoints, characterJoints, sampleSurface, frames, nearestSegment, carry, girths, posedJoints,
  poseMoves, applyMove} from './autorig.js';

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
  for (const [k, path] of (info.textures || []).entries()) {
    const t = new THREE.CanvasTexture(await createImageBitmap(new Blob([files[path]], {type: 'image/png'})));
    t.flipY = false; t.colorSpace = THREE.SRGBColorSpace; applyWrap(t, info.wrap?.[k] || 'rr');   // (a pack from before "wrap": the game repeats)
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

// A texture's wrap mode, across and down, as two letters - r repeat, c clamp ("Extend" in Blender), m mirror: from the .glb's sampler,
// kept in model.json's "wrap" (one per texture) so the game draws it the same. UVs past 0..1 are how some models tile a body texture.
const WRAPS = {r: THREE.RepeatWrapping, c: THREE.ClampToEdgeWrapping, m: THREE.MirroredRepeatWrapping};
const wrapCode = w => w === THREE.MirroredRepeatWrapping ? 'm' : w === THREE.ClampToEdgeWrapping ? 'c' : 'r';
function applyWrap(t, code) { if (code) { t.wrapS = WRAPS[code[0]]; t.wrapT = WRAPS[code[1]]; } }

export function createModelEditor({holder, camera, controls, renderer, $, setStatus, download, outfitsOf, textureFor}) {
  const ed = {outfit: null, pieces: null, skel: null, rig: null, src: null, view: null, posed: false,
    joints: null, modelSkel: null, modelRig: null, dots: null};

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

  // ---- the model file: a .glb / .gltf, an .fbx, an .obj (with its .mtl and pictures picked with it) - or a .zip of them ----
  // The other files picked (or in the zip) are its pictures and its .mtl: found by name, wherever the model says they are.
  async function readModel(list) {
    const files = new Map();   // (lower-case name -> bytes)
    for (const f of list) {
      const bytes = new Uint8Array(await f.arrayBuffer());
      if (/\.zip$/i.test(f.name)) {
        for (const [path, data] of Object.entries(unzipSync(bytes))) if (data.length) files.set(path.split('/').pop().toLowerCase(), data);
      } else {
        files.set(f.name.toLowerCase(), bytes);
      }
    }
    const names = [...files.keys()], pick = re => names.find(n => re.test(n));
    const main = pick(/\.glb$/) || pick(/\.gltf$/) || pick(/\.fbx$/) || pick(/\.obj$/);
    if (!main) throw new Error('no .glb, .gltf, .fbx or .obj in what was picked');
    // (.tga and .dds pictures: made PNGs here, under their own names - every loader then reads them as any picture;
    //  one that can't be read is left out, and so reported missing)
    const unreadable = [];
    for (const n of names) if (NEEDS_TURNING.test(n)) {
      const png = await toPng(n, files.get(n));
      if (png) files.set(n, png); else { files.delete(n); unreadable.push(n); }
    }
    // (the pictures it asks for that weren't picked: its parts there come out in a plain colour - told, by name)
    const missing = new Set(), failed = new Set();
    const urls = new Map(), url = n => {
      if (/^(blob|data):/i.test(n)) return null;   // (its own, inside the file - an .fbx's, a .glb's)
      const key = n.split(/[\\/]/).pop().toLowerCase();
      if (!files.has(key)) { if (/\.(png|jpe?g|tga|dds|bmp|gif|webp|tiff?|psd)$/i.test(key)) missing.add(n.split(/[\\/]/).pop()); return null; }
      if (!urls.has(key)) urls.set(key, URL.createObjectURL(new Blob([files.get(key)])));
      return urls.get(key);
    };
    const manager = new THREE.LoadingManager();
    manager.setURLModifier(u => url(decodeURIComponent(u)) || u);
    manager.addHandler(/\.(tga|dds)$/i, new THREE.TextureLoader(manager));   // (they're PNGs by now)
    let pending = 0;
    const loaded = new Promise(r => { manager.onLoad = r; });
    manager.onStart = () => { pending++; };
    manager.onError = u => { failed.add(decodeURIComponent(u).split(/[\\/]/).pop()); };
    const data = files.get(main), text = () => new TextDecoder().decode(data);
    let scene;
    if (/\.gl(b|tf)$/.test(main)) scene = (await new GLTFLoader(manager).parseAsync(data.buffer.slice(data.byteOffset, data.byteOffset + data.byteLength), '')).scene;
    else if (/\.fbx$/.test(main)) scene = new FBXLoader(manager).parse(data.buffer.slice(data.byteOffset, data.byteOffset + data.byteLength), '');
    else {
      const obj = new OBJLoader(manager), lib = /^mtllib\s+(.+)$/m.exec(text());
      const mtl = (lib && files.get(lib[1].trim().split(/[\\/]/).pop().toLowerCase())) || files.get(pick(/\.mtl$/));
      if (mtl) {
        const mats = new MTLLoader(manager).parse(new TextDecoder().decode(mtl), '');
        mats.preload();
        obj.setMaterials(mats);
      }
      scene = obj.parse(text());
    }
    if (pending) await Promise.race([loaded, new Promise(r => setTimeout(r, 8000))]);   // (its pictures read)
    await new Promise(r => setTimeout(r, 50));
    // (and any material whose picture came to nothing - a broken file, an .fbx's .psd)
    let blank = 0;
    scene.traverse(o => {
      if (!o.isMesh) return;
      for (const m of Array.isArray(o.material) ? o.material : [o.material]) {
        const im = m?.map?.image;
        if (m?.map && !(im && (im.width || im.naturalWidth))) blank++;
      }
    });
    const lost = [...new Set([...missing, ...unreadable, ...[...failed].filter(n => !/^blob:/i.test(n))])];
    return {scene, name: main.replace(/\.(glb|gltf|fbx|obj)$/i, ''), files: names, lost, blank};
  }
  $('importModel').onchange = async e => {
    const list = [...e.target.files];
    e.target.value = '';
    if (!list.length || !ed.skel) return;
    try {
      const r = await readModel(list), g = {scene: r.scene}, f = {name: r.name};
      g.scene.updateMatrixWorld(true);
      const parts = [];
      let objects = 0;
      const upFromBottom = new Set();   // (pictures whose coordinates count from the bottom - kept after their flag is cleared)
      g.scene.traverse(o => {
        if (!o.isMesh || !o.geometry?.attributes.position) return;
        const mats = Array.isArray(o.material) ? o.material : [o.material];
        let geo = o.geometry.clone();
        if (!o.isSkinnedMesh) geo.applyMatrix4(o.matrixWorld);   // (a rigged one: as bound - its bind pose)
        if (!geo.index) geo.setIndex([...Array(geo.attributes.position.count).keys()]);
        if (!geo.attributes.normal) geo.computeVertexNormals();
        // (an .obj's / .fbx's texture coordinates count up from the picture's bottom - their loaders flip the picture to
        //  match; the pack keeps the picture as it is, as a .glb's are, so they're flipped here instead: v -> 1 - v)
        for (const m of mats) if (m?.map?.flipY) { upFromBottom.add(m.map); m.map.flipY = false; m.map.needsUpdate = true; }
        if (geo.attributes.uv && mats.some(m => upFromBottom.has(m?.map))) {
          const uv = geo.attributes.uv;
          for (let i = 0; i < uv.count; i++) uv.setY(i, 1 - uv.getY(i));
        }
        const groups = geo.groups.length ? geo.groups : [{start: 0, count: geo.index.count, materialIndex: 0}];
        const object = {name: o.name || `part ${objects + 1}`, n: objects++, off: false};
        for (const gr of groups) {
          const m = mats[gr.materialIndex] || mats[0];
          // its own bones, if it's rigged on this character's skeleton (bones named bone00, bone01... - see Emotes)
          let boneOf = null;
          if (o.isSkinnedMesh) boneOf = o.skeleton.bones.map(b => (/^bone(\d+)$/.exec(b.name) || [])[1]).map(x => (x === undefined ? -1 : +x));
          parts.push({geo, start: gr.start, count: gr.count, color: m?.color ? m.color.clone() : new THREE.Color(1, 1, 1),
            map: m?.map?.image || null, wrap: m?.map ? wrapCode(m.map.wrapS) + wrapCode(m.map.wrapT) : null,
            boneOf: boneOf && boneOf.every(x => x >= 0) ? boneOf : null, object});
        }
      });
      if (!parts.length) throw new Error('no meshes in it');
      const geos = [...new Set(parts.map(p => p.geo))];   // (one mesh with several materials: several parts, one geometry)
      // lying down - much deeper than it is tall: made with z up (Blender's own axes, an .fbx that kept them) - stood up,
      // its top to +y and its front (Blender's -y) to +z, as the rest of the Studio has a model. Not one rigged on this
      // character already: that's in the character's own space.
      let stoodUp = false;
      if (!parts.some(p => p.boneOf)) {
        // (told by its biggest piece: a prop beside it - Panda Amy's .fbx has a ball round her a hundred times her size -
        //  isn't the model)
        const main = geos.reduce((a, g) => (g.attributes.position.count > a.attributes.position.count ? g : a));
        const s = new THREE.Box3().setFromBufferAttribute(main.attributes.position).getSize(new THREE.Vector3());
        if (s.z > s.y * 1.25) {
          const up = new THREE.Matrix4().makeRotationX(-Math.PI / 2);
          for (const g of geos) g.applyMatrix4(up);
          stoodUp = true;
        }
      }
      const verts = geos.reduce((n, g) => n + g.attributes.position.count, 0);
      if (verts > 65535) throw new Error(`it has ${verts.toLocaleString()} vertices - 65,535 at most`);
      ed.src = {parts, all: parts, name: f.name};
      ed.joints = null;
      showParts();
      if (!$('modelName').value) $('modelName').value = ed.src.name;
      // (inside the character's box, about its size or smaller: made on it already - its place kept, and most likely
      //  pieces worn over it; above it, room for a tall hat)
      const s = srcBox(), c = charBox(), pad = c.getSize(new THREE.Vector3()).multiplyScalar(0.15);
      const made = new THREE.Box3(c.min.clone().sub(pad), c.max.clone().add(pad).add(new THREE.Vector3(0, pad.y, 0))).containsBox(s);
      $('fitKeep').checked = made && !$('fitRig').checked;   // (rigged on its own shape: the auto-rig's choice stands)
      $('modelOver').checked = made && !$('fitRig').checked;
      refit(true);
      $('saveModel').disabled = false;
      setStatus(`Brought in "${f.name}" (${r.files.join(', ')}): ${parts.length} part${parts.length > 1 ? 's' : ''}, ${verts.toLocaleString()} vertices` +
        (parts.every(p => p.boneOf) ? ' - rigged on this character: its own weights' : ' - fitted, bones copied from the character') + '.' +
        (stoodUp ? ' It was lying down (made with z up) - stood up.' : '') +
        (r.lost.length ? ` MISSING PICTURES: ${r.lost.join(', ')} - pick ${r.lost.length > 1 ? 'them' : 'it'} with the model (or put ${r.lost.length > 1 ? 'them' : 'it'} in its .zip); till then those parts are a plain colour.`
          : r.blank ? ` ${r.blank} of its pictures couldn't be read - those parts are a plain colour.` : ''));
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
  $('fitRig').onchange = () => {
    const on = $('fitRig').checked;
    $('rigRow').hidden = $('rigHelp').hidden = !on;
    if (on) { $('fitKeep').checked = false; $('modelOver').checked = false; }
    drawPreview();
  };
  $('rigFind').onclick = () => { ed.joints = null; drawPreview(); };

  // its parts (the objects in the file), each one kept or left out - a stand, a ball around it, a backdrop
  function showParts() {
    const box = $('modelParts'), objects = [...new Set(ed.src.all.map(p => p.object))];
    box.replaceChildren(box.firstElementChild);   // (its heading kept)
    box.hidden = objects.length < 2;
    for (const ob of objects) {
      const verts = new Set(ed.src.all.filter(p => p.object === ob).map(p => p.geo)), n = [...verts].reduce((k, g) => k + g.attributes.position.count, 0);
      const label = document.createElement('label'), tick = document.createElement('input');
      label.className = 'muted';
      label.style.display = 'block';
      tick.type = 'checkbox';
      tick.checked = !ob.off;
      tick.onchange = () => {
        ob.off = !tick.checked;
        const kept = ed.src.all.filter(p => !p.object.off);
        if (!kept.length) { ob.off = false; tick.checked = true; return; }
        ed.src.parts = kept;
        ed.joints = null;
        refit(true);
      };
      label.append(tick, ` ${ob.name} (${n.toLocaleString()} vertices)`);
      box.append(label);
    }
  }
  $('rigEdit').onchange = () => drawPreview();
  const editingJoints = () => autoRig() && $('rigEdit').checked;

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
    if (autoRig()) {   // (rigged on its own shape: as tall as the character, nothing stretched - and back, for a dragged joint)
      const k = c.getSize(new THREE.Vector3()).y / Math.max(s.getSize(new THREE.Vector3()).y, 1e-6) * size;
      const base = new THREE.Vector3(cc.x, c.min.y, cc.z).add(off), qi = q.clone().invert();
      return {
        map: (v, n) => ({pos: local(v).multiplyScalar(k).add(base), nrm: n.clone().applyQuaternion(q)}),
        unmap: p => p.clone().sub(base).multiplyScalar(1 / k).applyQuaternion(qi).add(new THREE.Vector3(sc.x, s.min.y, sc.z)),
      };
    }
    // the character's shape: its own points, its arm height from its skeleton
    const cpts = [];
    for (const p of ed.pieces) {
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
  // (moved: the character's vertices put somewhere else - the auto-rig's: on the model's shape - one array a piece)
  function charGrid(cell, moved = null) {
    const grid = new Map(), key = (x, y, z) => x + ',' + y + ',' + z;
    for (const [pk, p] of ed.pieces.entries()) {
      if (!p.skin) continue;
      const P = moved ? moved[pk] : p.position;
      for (let i = 0; i < p.position.length / 3; i++) {
        const x = P[i * 3], y = P[i * 3 + 1], z = P[i * 3 + 2];
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
    for (let r = n ? 2 : 1; r <= 48; r++) {   // (rings out until some are found - a big model's hair can be far from any)
      const near = [];
      for (let x = cx - r; x <= cx + r; x++) for (let y = cy - r; y <= cy + r; y++) for (let z = cz - r; z <= cz + r; z++)
        for (const v of G.grid.get(G.key(x, y, z)) || [])
          if (!side || (v.x - G.cx) * side >= 0) near.push([dist(v), v]);
      if ((near.length < (n ? 6 : 4) && r < 8) || (!near.length && r < 48)) continue;
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

  // ---- worn over the character: none of the character showing through it ------------------------------------------
  // Where the model's surface is inside the character, or nearer than CLEAR, and that bit of the character is drawn (not
  // among the triangles left out under the model: a leg inside a boot, the head inside a hat's crown), it goes out
  // along the character's normal there to CLEAR off it - a vest whose back and shoulders sat inside Peter's showed his
  // shirt through it. The model's vertices within CARRY of one moved go with it (all the way to half of it, then less, to
  // none at CARRY): a shell keeps its thickness, a button or a pocket stays on it. Moves of more than PUSH are left (a part meant to be
  // inside). The character's triangles in a grid, as charGrid does its vertices.
  function pushOut(m) {
    const CLEAR = 0.02, CARRY = 0.06, PUSH = 0.15, cell = 0.05;   // (CLEAR: past the character's outline, 0.007 out)
    const hidden = new Map(hiddenUnder(m).map(h => [h.piece, new Set(h.ranges.flatMap(([s, n]) => Array.from({length: n}, (_, k) => s + k)))]));
    const grid = new Map(), key = (x, y, z) => x + ',' + y + ',' + z;
    for (const p of ed.pieces) {
      const P = p.position, N = p.normal, I = p.index, gone = hidden.get(p);
      for (let t = 0; t < I.length / 3; t++) {
        if (gone?.has(t)) continue;
        const c = [I[t * 3], I[t * 3 + 1], I[t * 3 + 2]];
        const tri = {tri: new THREE.Triangle(...c.map(i => new THREE.Vector3().fromArray(P, i * 3))),
          n: c.map(i => new THREE.Vector3().fromArray(N, i * 3))};
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
    const q = new THREE.Vector3(), bary = new THREE.Vector3();
    const outOf = p => {   // (how far out of the character: from its nearest drawn point, along its normal there)
      let best = null, bd = Infinity;
      const cx = Math.floor(p.x / cell), cy = Math.floor(p.y / cell), cz = Math.floor(p.z / cell);
      for (let r = 1; r <= 3 && !best; r++)
        for (let x = cx - r; x <= cx + r; x++) for (let y = cy - r; y <= cy + r; y++) for (let z = cz - r; z <= cz + r; z++)
          for (const t of grid.get(key(x, y, z)) || []) {
            t.tri.closestPointToPoint(p, q);
            const d = q.distanceToSquared(p);
            if (d < bd) { bd = d; best = {t, at: q.clone()}; }
          }
      if (!best) return null;
      best.t.tri.getBarycoord(best.at, bary);
      const n = best.t.n[0].clone().multiplyScalar(bary.x).add(best.t.n[1].clone().multiplyScalar(bary.y))
        .add(best.t.n[2].clone().multiplyScalar(bary.z)).normalize();
      return {d: p.clone().sub(best.at).dot(n), n};
    };
    let moved = 0;
    for (let pass = 0; pass < 3; pass++) {   // (again: neighbours moved along slightly different normals)
      const pushes = [];
      for (const v of m.verts) {
        const o = outOf(v.pos);
        if (o && o.d < CLEAR && CLEAR - o.d <= PUSH) pushes.push({at: v.pos.clone(), by: o.n.multiplyScalar(CLEAR - o.d)});
      }
      if (!pushes.length) break;
      for (const v of m.verts) {   // (each vertex: the biggest of the moves near it, fading with distance)
        let best = null, size = 0;
        for (const p of pushes) {
          const d = p.at.distanceTo(v.pos);
          if (d >= CARRY) continue;
          const s = p.by.length() * (d < CARRY / 2 ? 1 : 2 - 2 * d / CARRY);
          if (s > size) { size = s; best = p.by.clone().setLength(s); }
        }
        if (best) { v.pos.add(best); moved++; }
      }
    }
    return moved;
  }

  // A part worn on the trunk (most of its weight on the spine, from the hips up to where the arms join) doesn't follow
  // the limbs: weight on a limb's bones past its first (the collarbone, the thigh, the neck) goes to that first bone,
  // and the first takes at most LIMB of a vertex (the rest to the spine bone it hangs from). The vest's armholes and
  // sides took the upper arms' weights from under them, and swung with the arms - onto the gun. The trunk: the spine
  // bones up to the one both hands hang from (a hand: a bone with four children or more - its fingers - the furthest
  // out on its side; the head has as many, its face's).
  function keepToTheTrunk(m) {   // (part by part: a model's parts can all be one geometry)
    const LIMB = 0.35, {parent, count} = ed.skel;
    const kids = Array.from({length: count}, () => 0);
    parent.forEach(p => { if (p >= 0) kids[p]++; });
    const xs = ed.skel.bind.map(b => new THREE.Vector3().setFromMatrixPosition(b).x);
    const many = kids.map((k, i) => k >= 4 ? i : -1).filter(i => i >= 0);   // (the head too: its face bones)
    const hands = [many.reduce((a, b) => xs[b] > xs[a] ? b : a, many[0]), many.reduce((a, b) => xs[b] < xs[a] ? b : a, many[0])];
    if (!many.length || !(xs[hands[0]] > 0.1) || !(xs[hands[1]] < -0.1)) return;   // (one far out on each side)
    const up = b => { const out = []; for (; b >= 0; b = parent[b]) out.push(b); return out; };
    const a = up(hands[0]), b = new Set(up(hands[1]));
    const join = a.find(x => b.has(x));
    if (join === undefined) return;
    const trunk = new Set(up(join));
    const first = bone => { while (bone >= 0 && !trunk.has(parent[bone])) bone = parent[bone]; return bone; };
    const verts = m.verts;
    for (const part of m.parts) {
      const mine = new Set();
      for (let k = part.first; k < part.first + part.count; k++) mine.add(m.idx[k]);
      let all = 0, onTrunk = 0;
      for (const i of mine) for (const [bone, w] of verts[i].bw) { all += w; if (trunk.has(bone)) onTrunk += w; }
      if (!all || onTrunk < all * 0.5) continue;
      for (const i of mine) {
        const acc = new Map(), total = verts[i].bw.reduce((t, [, w]) => t + w, 0) || 1;
        for (const [bone, w] of verts[i].bw) {
          const f = trunk.has(bone) ? bone : first(bone);
          if (f < 0) { acc.set(bone, (acc.get(bone) || 0) + w); continue; }
          acc.set(f, (acc.get(f) || 0) + w);
        }
        for (const [bone, w] of [...acc]) {
          if (trunk.has(bone) || w <= total * LIMB) continue;
          acc.set(bone, total * LIMB);
          acc.set(parent[bone], (acc.get(parent[bone]) || 0) + w - total * LIMB);
        }
        verts[i].bw = [...acc.entries()].sort((x, y) => y[1] - x[1]).slice(0, 4);
      }
    }
  }

  // ---- the auto-rig (autorig.js): its joints, the character's mesh put on its shape for the weights, its own skeleton -----
  function autoRig() { return $('fitRig').checked && ed.src && !ed.src.parts.every(p => p.boneOf) && !$('fitKeep').checked; }
  // its joints where it's placed now (kept in its own space: a slider moves them with it)
  function jointsNow(P) {
    if (!ed.joints) {   // (found from its shape: points over its surface, standing on y 0, its middle at x 0)
      const tris = [];
      for (const sp of ed.src.parts) {
        const a = sp.geo.attributes.position, I = sp.geo.index;
        for (let k = sp.start; k + 2 < sp.start + sp.count; k += 3) tris.push([a, I.getX(k), I.getX(k + 1), I.getX(k + 2)]);
      }
      const pts = sampleSurface(tris.length, (k, A, B, C) => {
        const [a, i, j, l] = tris[k];
        A.copy(P.map(new THREE.Vector3().fromBufferAttribute(a, i), new THREE.Vector3(0, 1, 0)).pos);
        B.copy(P.map(new THREE.Vector3().fromBufferAttribute(a, j), new THREE.Vector3(0, 1, 0)).pos);
        C.copy(P.map(new THREE.Vector3().fromBufferAttribute(a, l), new THREE.Vector3(0, 1, 0)).pos);
      });
      const c = charBox(), base = new THREE.Vector3(c.getCenter(new THREE.Vector3()).x, c.min.y, c.getCenter(new THREE.Vector3()).z);
      const J = guessJoints(pts.map(p => p.sub(base)));
      ed.joints = Object.fromEntries(LANDMARKS.map(k => [k, P.unmap(J[k].add(base))]));
      ed.samples = null;
    }
    return Object.fromEntries(LANDMARKS.map(k => [k, P.map(ed.joints[k], new THREE.Vector3(0, 1, 0)).pos]));
  }
  // (rigged on its own shape: what's in its head follows the head alone - the character's jaw and face bones are for its
  //  own mouth and eyes; a model's that has none, its cheeks and hair stretched as the character talked)
  function headOnly(bw) {
    const head = ed.skel.names.findIndex(n => /^Head(?! 2)/.test(n));
    if (head < 0) return bw;
    const under = b => { for (let p = ed.skel.parent[b]; p >= 0; p = ed.skel.parent[p]) if (p === head) return true; return false; };
    const acc = new Map();
    for (const [b, w] of bw) { const k = under(b) ? head : b; acc.set(k, (acc.get(k) || 0) + w); }
    return [...acc.entries()].sort((x, y) => y[1] - x[1]);
  }
  function rigOnItsShape(P) {
    const Jm = jointsNow(P), Jc = characterJoints(ed.skel, ed.pieces);
    const Fc = frames(Jc), Fm = frames(Jm);
    // how thick it is about each segment, against the character (its own vertices; the character's)
    const mpts = [];
    for (const sp of ed.src.parts) {
      const a = sp.geo.attributes.position;
      for (let i = 0; i < a.count; i += Math.max(1, Math.floor(a.count / 6000))) mpts.push(P.map(new THREE.Vector3().fromBufferAttribute(a, i), new THREE.Vector3(0, 1, 0)).pos);
    }
    const cpts = [];
    for (const p of ed.pieces) for (let i = 0; i < p.position.length; i += 3 * Math.max(1, Math.floor(p.position.length / 3 / 3000))) cpts.push(new THREE.Vector3().fromArray(p.position, i));
    const Gc = girths(Fc, cpts), Gm = girths(Fm, mpts);
    // each bone's segment: the nearest on its own side (a middle bone: the middle's)
    const pos = b => new THREE.Vector3().setFromMatrixPosition(ed.skel.bind[b]);
    const H = Jc.top.y - Math.min(Jc.toeL.y, Jc.toeR.y);
    const sideOf = b => { const m = / ([LR])\b/.exec(ed.skel.names[b]); return m ? m[1] : Math.abs(pos(b).x - Jc.pelvis.x) > H * 0.03 ? (pos(b).x > Jc.pelvis.x ? 'L' : 'R') : ''; };
    const segOf = ed.skel.parent.map((_, b) => {
      const sd = sideOf(b);
      return nearestSegment(Fc, pos(b), k => (sd ? SEGMENTS[k][1].endsWith(sd) : k < 4)).k;
    });
    // the character's vertices on the model's shape: each along its main bone's segment
    const moved = ed.pieces.map(p => {
      const out = new Float32Array(p.position.length);
      for (let i = 0; i < p.position.length / 3; i++) {
        let bone = 0, w = -1;
        if (p.skin) for (let j = 0; j < 4; j++) if (p.skin.weight[i * 4 + j] > w) { w = p.skin.weight[i * 4 + j]; bone = p.skin.index[i * 4 + j]; }
        const q = carry(new THREE.Vector3().fromArray(p.position, i * 3), segOf[bone], Fc, Fm, Gc, Gm);
        out.set([q.x, q.y, q.z], i * 3);
      }
      return out;
    });
    // its own skeleton: the character's bones put on its shape in the character's pose (each turned as the character's)
    const Pj = posedJoints(Jm, Jc), Fp = frames(Pj), moves = poseMoves(Jm, Pj);
    const count = ed.skel.count, bind = [], invBind = [], local = [];
    for (let b = 0; b < count; b++) {
      const t = new THREE.Vector3(), r = new THREE.Quaternion(), sc = new THREE.Vector3();
      ed.skel.bind[b].decompose(t, r, sc);
      const at = ed.skel.parent[b] < 0 ? t : carry(t, segOf[b], Fc, Fp, Gc, Gm);
      bind.push(new THREE.Matrix4().compose(at, r, new THREE.Vector3(1, 1, 1)));
    }
    for (let b = 0; b < count; b++) {
      invBind.push(bind[b].clone().invert());
      const par = ed.skel.parent[b];
      local.push(par < 0 ? bind[b].clone() : bind[par].clone().invert().multiply(bind[b]));
    }
    const hips = ed.skel.names.findIndex(n => /^Hips/.test(n)), root = Math.max(0, ed.skel.parent[hips]);
    const hy = new THREE.Vector3().setFromMatrixPosition(ed.skel.bind[hips]).y - new THREE.Vector3().setFromMatrixPosition(ed.skel.bind[root]).y;
    const my = new THREE.Vector3().setFromMatrixPosition(bind[hips]).y - new THREE.Vector3().setFromMatrixPosition(bind[root]).y;
    ed.modelSkel = {count, parent: ed.skel.parent, names: ed.skel.names, bind, invBind, local, hipScale: hips >= 0 && hy > 1e-4 ? my / hy : 1};
    return {
      moved,
      pose(verts) {   // (each vertex moved as its bones' segments are, by its weights)
        for (const v of verts) {
          if (!v.bw.length) continue;   // (none found: left where it is, not dropped to the floor)
          const tot = v.bw.reduce((t, [, w]) => t + w, 0) || 1, p = new THREE.Vector3(), n = new THREE.Vector3();
          for (const [bone, w] of v.bw) {
            const mv = moves[segOf[bone]] || moves[0];
            p.addScaledVector(applyMove(mv, v.pos), w / tot);
            n.addScaledVector(v.nrm.clone().applyMatrix4(mv.R), w / tot);
          }
          v.pos = p; v.nrm = n.normalize();
        }
      },
    };
  }
  // ---- the joints' dots: shown on it, dragged where they should be ------------------------------------------------------
  function showDots() {
    if (ed.dots) { holder.remove(ed.dots); ed.dots = null; }
    if (!editingJoints() || !ed.src || !ed.joints) return;
    const P = placer(), J = jointsNow(P), H = charBox().getSize(new THREE.Vector3()).y;
    ed.dots = new THREE.Group();
    for (const k of LANDMARKS) {
      const col = k.endsWith('L') ? 0x4cd964 : k.endsWith('R') ? 0xff6b6b : 0xf5d33a;
      const m = new THREE.Mesh(new THREE.SphereGeometry(H * 0.014, 12, 8), new THREE.MeshBasicMaterial({color: col, depthTest: false}));
      m.renderOrder = 10;
      m.position.copy(J[k]);
      m.userData.joint = k;
      ed.dots.add(m);
    }
    const lines = [];
    for (const [a, b] of SEGMENTS) lines.push(J[a], J[b]);
    const seg = new THREE.LineSegments(new THREE.BufferGeometry().setFromPoints(lines), new THREE.LineBasicMaterial({color: 0xffffff, depthTest: false, transparent: true, opacity: 0.6}));
    seg.renderOrder = 9;
    ed.dots.add(seg);
    holder.add(ed.dots);
  }
  {   // (dragging one: on the plane through it facing the camera; "both sides together" moves its other side's mirrored)
    const ray = new THREE.Raycaster(), plane = new THREE.Plane(), hit = new THREE.Vector3();
    let drag = null;
    const ndc = e => {
      const r = renderer.domElement.getBoundingClientRect();
      return new THREE.Vector2(((e.clientX - r.left) / r.width) * 2 - 1, -((e.clientY - r.top) / r.height) * 2 + 1);
    };
    renderer.domElement.addEventListener('pointerdown', e => {
      if (!ed.dots || !ed.dots.parent) return;
      ray.setFromCamera(ndc(e), camera);
      const h = ray.intersectObjects(ed.dots.children.filter(c => c.userData.joint));
      if (!h.length) return;
      drag = h[0].object;
      plane.setFromNormalAndCoplanarPoint(camera.getWorldDirection(new THREE.Vector3()), drag.getWorldPosition(new THREE.Vector3()));
      controls.enabled = false;
      e.stopPropagation();
    }, true);
    renderer.domElement.addEventListener('pointermove', e => {
      if (!drag) return;
      ray.setFromCamera(ndc(e), camera);
      if (!ray.ray.intersectPlane(plane, hit)) return;
      drag.position.copy(drag.parent.worldToLocal(hit.clone()));
      const k = drag.userData.joint, other = k.endsWith('L') ? k.slice(0, -1) + 'R' : k.endsWith('R') ? k.slice(0, -1) + 'L' : null;
      if (other && $('rigMirror').checked) {
        const o = ed.dots.children.find(c => c.userData.joint === other), cx = charBox().getCenter(new THREE.Vector3()).x;
        o.position.set(2 * cx - drag.position.x, drag.position.y, drag.position.z);
      }
      if (!other) drag.position.x = charBox().getCenter(new THREE.Vector3()).x;   // (a middle joint stays in the middle)
    });
    window.addEventListener('pointerup', () => {
      if (!drag) return;
      const P = placer();
      for (const c of ed.dots.children) if (c.userData.joint) ed.joints[c.userData.joint] = P.unmap(c.position.clone());
      drag = null;
      controls.enabled = true;
      drawPreview();
    });
  }

  // the model as the game will have it: parts with vertices (character space, bones) and their textures
  function prepare() {
    const P = placer(), rigged = ed.src.parts.every(p => p.boneOf);
    const R = autoRig() ? rigOnItsShape(P) : null;
    const G = rigged ? null : charGrid(charBox().getSize(new THREE.Vector3()).y / 40, R ? R.moved : null);
    const verts = [], idx = [], parts = [], textures = [], wraps = [], palette = [];
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
            if (R) bw = headOnly(bw);
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
        tex = textures.findIndex((t, k) => t === sp.map && wraps[k] === sp.wrap);   // (the same picture, wrapped another way: one more)
        if (tex < 0) { textures.push(sp.map); wraps.push(sp.wrap); tex = textures.length - 1; }
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
    if ($('modelOver').checked) pushOut({verts, idx, parts});
    // (a triangle wound against its own normals - left by the modeller's meshing, or folded moving it out - turned
    //  round: the game culls it as a back face, a hole, and our black outline - the model inflated, inside out - shows
    //  it as a black speck on the surface)
    const ea = new THREE.Vector3(), eb = new THREE.Vector3(), fn = new THREE.Vector3();
    for (let k = 0; k + 2 < idx.length; k += 3) {
      const A = verts[idx[k]], B = verts[idx[k + 1]], C = verts[idx[k + 2]];
      fn.crossVectors(ea.subVectors(B.pos, A.pos), eb.subVectors(C.pos, A.pos));
      if (fn.dot(A.nrm) + fn.dot(B.nrm) + fn.dot(C.nrm) < 0) [idx[k + 1], idx[k + 2]] = [idx[k + 2], idx[k + 1]];
    }
    if (G && $('modelOver').checked) { keepToTheTrunk({verts, idx, parts}); ride(verts, pieces); }
    const standing = R ? verts.map(v => v.pos.clone()) : null;   // (as it stands: the joints are shown on it)
    if (R) R.pose(verts);   // (into the character's own pose - its rest T-pose - its own shape kept)
    // (the palette: a color an 8 x 8 cell, 32 a row - the game's 4 x 4 compressed blocks and smaller mip levels keep it)
    if (palette.length > 1024) throw new Error('too many colors (' + palette.length + ') - 1,024 at most without a texture');
    if (palette.length) {
      textures.push('palette'); wraps.push('cc');
      for (const v of verts) if (v.paletteOf !== undefined) v.uv = [((v.paletteOf % 32) + 0.5) / 32, (Math.floor(v.paletteOf / 32) + 0.5) / 32];
    }
    for (const p of parts) p.texture = p.tex === 'palette' ? textures.length - 1 : p.tex;
    for (const v of verts) v.uv = v.uv || [0, 0];
    return {verts, idx, parts, textures, wraps, palette, standing};
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
    if (ed.modelRig) { holder.remove(ed.modelRig.root); ed.modelRig = null; }
    if (!ed.src || !ed.rig) { showDots(); return; }
    let m;
    try { m = prepare(); } catch (err) { setStatus(err.message); return; }
    ed.prepared = m;
    if (autoRig() && ed.modelSkel) {   // (the model's own skeleton: its bones where its joints are)
      ed.modelRig = makeRig(ed.modelSkel);
      holder.add(ed.modelRig.root);
      if (ed.posed) posing(ed.modelRig, ed.modelSkel);
    }
    const view = new THREE.Group();
    const texs = m.textures.map((t, k) => { const tx = new THREE.CanvasTexture(textureCanvas(t, m.palette)); tx.flipY = false; tx.colorSpace = THREE.SRGBColorSpace; applyWrap(tx, m.wraps[k]); return tx; });
    const n = m.verts.length, pos = new Float32Array(n * 3), nrm = new Float32Array(n * 3), uv = new Float32Array(n * 2);
    const si = new Uint16Array(n * 4), sw = new Float32Array(n * 4);
    const showStanding = editingJoints() && m.standing;   // (the joints shown: it as it stands, not rigged)
    m.verts.forEach((v, i) => {
      const at = showStanding ? m.standing[i] : v.pos;
      pos.set([at.x, at.y, at.z], i * 3); nrm.set([v.nrm.x, v.nrm.y, v.nrm.z], i * 3); uv.set(v.uv, i * 2);
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
      const mat = new THREE.MeshLambertMaterial({map: texs[p.texture], side: THREE.DoubleSide, alphaTest: 0.3});
      if (showStanding) { const plain = new THREE.Mesh(g, mat); plain.frustumCulled = false; view.add(plain); continue; }
      const mesh = new THREE.SkinnedMesh(g, mat);
      mesh.bind(ed.modelRig && autoRig() ? ed.modelRig.skeleton : ed.rig.skeleton, new THREE.Matrix4());
      mesh.frustumCulled = false;
      view.add(mesh);
    }
    holder.add(view);
    ed.view = view;
    showDots();
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
  // (a rig, from its skeleton's rest: as the game turns the character's bones - the model's own bones turned the same)
  function posing(rig, skel) {
    const names = ed.skel.names;
    rig.bones.forEach((b, i) => { skel.local[i].decompose(b.position, b.quaternion, b.scale); });
    if (ed.posed) rig.bones.forEach((b, i) => {
      if (/^Arm [LR] 2 /.test(names[i])) b.rotateZ(/ L /.test(names[i]) ? -0.9 : 0.9);
      if (/^Arm [LR] [34] /.test(names[i])) b.rotateY(/ L /.test(names[i]) ? 0.5 : -0.5);
      if (/^Head /.test(names[i])) b.rotateY(0.5);
      if (/^Leg L 1 /.test(names[i])) b.rotateZ(0.4);
      if (/^Leg R 2 /.test(names[i])) b.rotateX(0.7);
    });
  }
  $('modelPose').onclick = () => {
    if (!ed.rig) return;
    ed.posed = !ed.posed;
    posing(ed.rig, ed.skel);
    if (ed.modelRig) posing(ed.modelRig, ed.modelSkel);
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
    const own = autoRig() && ed.modelSkel;   // (rigged on its own shape: its own bones' places)
    for (const m4 of (own ? ed.modelSkel.invBind : ed.skel.invBind)) { const e = m4.elements; [0, 4, 8, 12, 1, 5, 9, 13, 2, 6, 10, 14].forEach(k => f32(e[k])); }
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
      bones: S + 1, textures: pngs, wrap: m.wraps, over: $('modelOver').checked, tool: 'FGUY Skin Studio'};
    if (own) { manifest.over = false; manifest.ownShape = {hips: +ed.modelSkel.hipScale.toFixed(4)}; }   // (models.cpp: its own bones' lengths)
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
