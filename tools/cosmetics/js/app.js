// FGUY Skin Studio: pick the game folder, a character and outfit, see it in 3D, recolor / repaint its textures, save a
// skin pack (.fgskin: a zip with skin.json - what was done to each texture, no game art) for the game's cosmetics mod - or, on the
// Emotes tab, pose the character on a timeline and save an emote pack (.fgemote, emotes.js) for the emote mod.
import * as THREE from 'three';
import {OrbitControls} from 'three/addons/controls/OrbitControls.js';
import {zipSync, unzipSync, strToU8, strFromU8} from 'fflate';
import {Ho} from './ho.js';
import {decodeDds} from './dds.js';
import {loadModel, loadSkeleton, outfitsOf, pictureFor, CHARACTERS} from './model.js';
import {createEmoteEditor} from './emotes.js';
import {createModelEditor, modelPackGroup} from './models.js';

const $ = id => document.getElementById(id);
const NICE = {AdamWest: 'Adam West'};
const state = {ho: null, outfits: [], current: null, textures: new Map(), selected: null, picked: null, mode: 'skins'};
let emotes = null, models = null;   // (the emote and model editors, made below)

// ---- 3D view ---------------------------------------------------------------------------------------------------
const view = $('view');
const renderer = new THREE.WebGLRenderer({antialias: true, alpha: true, preserveDrawingBuffer: true});
renderer.setPixelRatio(Math.min(devicePixelRatio, 2));
renderer.outputColorSpace = THREE.SRGBColorSpace;
view.prepend(renderer.domElement);
const scene = new THREE.Scene();
scene.add(new THREE.HemisphereLight(0xffffff, 0x445577, 2.2));
const sun = new THREE.DirectionalLight(0xffffff, 1.4);
sun.position.set(2, 3, 4);
scene.add(sun);
const camera = new THREE.PerspectiveCamera(32, 1, 0.01, 100);
camera.position.set(0, 0.6, 3);
const controls = new OrbitControls(camera, renderer.domElement);
controls.enableDamping = true;
const holder = new THREE.Group();
scene.add(holder);
function resize() {
  const w = view.clientWidth, h = view.clientHeight;
  renderer.setSize(w, h, false);
  camera.aspect = w / Math.max(h, 1);
  camera.updateProjectionMatrix();
}
new ResizeObserver(resize).observe(view);
const clock = new THREE.Clock();
// ---- the in-game look (the view's toggle): the game draws characters cel-shaded, with black ink outlines - its own
// round the character's pieces, ours (models.cpp) round a model's parts bar the smallest (laces, rivets: under 0.07
// across). So: toon shading in three flat tones on everything, an outline (the back faces, pushed out along the
// normals) on the same; their triangles a worn model hides stay hidden. Kept in step every frame - views are rebuilt
// as you work.
const look = {on: false};
look.ramp = new THREE.DataTexture(new Uint8Array([95, 95, 95, 255, 175, 175, 175, 255, 255, 255, 255, 255]), 3, 1);
look.ramp.minFilter = look.ramp.magFilter = THREE.NearestFilter;
look.ramp.needsUpdate = true;
look.line = new THREE.MeshBasicMaterial({color: 0x111111, side: THREE.BackSide});
look.line.onBeforeCompile = sh => {
  sh.vertexShader = sh.vertexShader.replace('#include <skinning_vertex>', `#include <skinning_vertex>
  #ifdef USE_SKINNING
    transformed += normalize(objectNormal) * 0.007;
  #else
    transformed += normalize(normal) * 0.007;
  #endif`);
};
function partSize(o) {   // (a part's size across: the vertices its triangles use - its geometry holds the model's all)
  if (o.userData.lookSize !== undefined) return o.userData.lookSize;
  const p = o.geometry.attributes.position, idx = o.geometry.index, box = new THREE.Box3(), v = new THREE.Vector3();
  if (idx) for (let k = 0; k < idx.count; k++) box.expandByPoint(v.fromBufferAttribute(p, idx.getX(k)));
  return (o.userData.lookSize = idx && idx.count ? box.getSize(v).length() : 0);
}
function syncLook() {
  const on = $('gameLook').checked;
  holder.traverse(o => {
    if (!o.isMesh || o.userData.lookLine) return;
    const m = o.material;
    if (on && m.isMeshLambertMaterial) {
      o.userData.lookPlain = m;
      o.material = new THREE.MeshToonMaterial({map: m.map, gradientMap: look.ramp, side: m.side, alphaTest: m.alphaTest});
      if (o.userData.piece || o.userData.texture || partSize(o) >= 0.07) {   // (outlined)
        const line = o.isSkinnedMesh ? new THREE.SkinnedMesh(o.geometry, look.line) : new THREE.Mesh(o.geometry, look.line);
        if (o.isSkinnedMesh) line.bind(o.skeleton, o.bindMatrix);
        line.userData.lookLine = true;
        line.frustumCulled = false;
        o.add(line);
      }
    } else if (!on && m.isMeshToonMaterial && o.userData.lookPlain) {
      m.dispose();
      o.material = o.userData.lookPlain;
      for (const c of o.children.filter(c => c.userData.lookLine)) o.remove(c);
    }
  });
}

renderer.setAnimationLoop(() => {
  const dt = clock.getDelta();
  syncLook();
  if (state.mode === 'emotes') emotes?.tick(dt);
  controls.update();
  renderer.render(scene, camera);
});

// ---- opening the game ------------------------------------------------------------------------------------------
async function openPackage(file) {
  setStatus('Reading ' + file.name + '...');
  try {
    state.ho = await Ho.open(file);
  } catch (e) {
    setStatus("Couldn't read it: " + e.message);
    return;
  }
  state.outfits = outfitsOf(state.ho);
  state.textures.clear();
  $('welcome').style.display = 'none';
  buildCharacterList(await library);
  setStatus(state.outfits.length + ' outfits found.');
  const first = state.outfits.find(o => o.character === 'Stewie' && o.outfit === 'Default') || state.outfits[0];
  if (first) selectOutfit(first);
}

$('openFolder').onclick = async () => {
  if (!window.showDirectoryPicker) { setStatus('This browser can\'t open folders - use "Or pick PLYS.ho".'); return; }
  try {
    let dir = await window.showDirectoryPicker({id: 'fguy', mode: 'read'});
    for (const part of ['FGUY', 'Global_Scenes', 'Players']) {
      try { dir = await dir.getDirectoryHandle(part); } catch { /* (they picked a folder further in) */ }
    }
    const file = await (await dir.getFileHandle('PLYS.ho')).getFile();
    await openPackage(file);
  } catch (e) {
    if (e.name !== 'AbortError') setStatus("That doesn't look like the game folder (no FGUY\\Global_Scenes\\Players\\PLYS.ho).");
  }
};
$('openFile').onchange = e => e.target.files[0] && openPackage(e.target.files[0]);

// Our skins (the FGUY Cosmetics mod's - no game art in them): library.json lists them, the packs are the mod's own files
// (dev_server.py hands out dist/FGUYMod/mods as /mods/). Each shows as a chip beside its character's outfits.
// Served some other way (a plain web server on this folder, no dev_server.py), they're fetched from the repository.
const REPO_RAW = 'https://raw.githubusercontent.com/alphain24/FGUY-Mod/main/';
const fetchOr = async (local, remote) => {
  try { const r = await fetch(local); if (r.ok) return r; } catch (e) { /* (not here) */ }
  return fetch(remote).catch(() => ({ok: false}));
};
const library = fetchOr('library.json', REPO_RAW + 'tools/cosmetics/library.json')
  .then(r => (r.ok ? r.json() : {skins: []})).catch(() => ({skins: []}));
async function openLibrarySkin(entry) {
  setStatus('Opening "' + entry.name + '"...');
  const file = encodeURIComponent(entry.file);
  const r = await fetchOr('mods/FGUYCosmetics/skins/' + file, REPO_RAW + 'dist/FGUYMod/mods/FGUYCosmetics/skins/' + file);
  if (!r.ok) { setStatus("Couldn't find " + entry.file + '.'); return; }
  await openPackBytes(new Uint8Array(await r.arrayBuffer()), entry.name);
  for (const c of document.querySelectorAll('.chip')) c.classList.toggle('on', c === entry.chip);
}

// An opened skin pack (from the library or a file) is left when another outfit or character is clicked: the textures
// it changed go back to the game's own, its model comes off - it doesn't follow you onto the next outfit.
function leavePack() {
  if (!state.fromPack) return;
  state.fromPack = false;
  for (const t of state.textures.values()) {
    if (!t.modified && !t.ops.length) continue;
    t.ctx.putImageData(t.original, 0, 0);
    t.ops = [];
    t.history = [];
    t.modified = false;
    t.tex.needsUpdate = true;
  }
  state.packModel = null;
  showPackModel();
  state.packPicture = null;
  showPackPicture();
  $('packName').value = '';
}

function buildCharacterList(lib = {skins: []}) {
  const box = $('chars');
  box.innerHTML = '';
  for (const ch of CHARACTERS) {
    const outfits = state.outfits.filter(o => o.character === ch);
    if (!outfits.length) continue;
    const b = document.createElement('button');
    b.className = 'item';
    b.textContent = NICE[ch] || ch;
    b.dataset.char = ch;
    const row = document.createElement('div');
    row.className = 'outfits';
    for (const o of outfits.sort((a, b) => (a.outfit === 'Default' ? -1 : b.outfit === 'Default' ? 1 : a.outfit.localeCompare(b.outfit)))) {
      const c = document.createElement('span');
      c.className = 'chip';
      c.textContent = o.outfit.replace(/([a-z])([A-Z])/g, '$1 $2');
      c.onclick = () => { leavePack(); selectOutfit(o); };
      o.chip = c;
      row.append(c);
    }
    for (const e of lib.skins.filter(x => x.character === ch)) {   // (ours)
      const c = document.createElement('span');
      c.className = 'chip ours';
      c.textContent = '\u2605 ' + e.name;
      c.title = 'FGUY Cosmetics skin' + (e.outfit ? ' (on ' + e.outfit + ')' : '') + ' - open it to see or change it';
      c.onclick = () => openLibrarySkin(e);
      e.chip = c;
      row.append(c);
    }
    b.onclick = () => { leavePack(); selectOutfit(outfits[0]); };
    box.append(b, row);
  }
}

// ---- a character -----------------------------------------------------------------------------------------------
async function textureFor(info) {
  const key = info.name;
  if (state.textures.has(key)) return state.textures.get(key);
  const dds = decodeDds(await info.blob.bytes());
  const canvas = document.createElement('canvas');
  canvas.width = dds.width; canvas.height = dds.height;
  const ctx = canvas.getContext('2d', {willReadFrequently: true});
  const original = new ImageData(dds.rgba, dds.width, dds.height);
  ctx.putImageData(original, 0, 0);
  const tex = new THREE.CanvasTexture(canvas);
  tex.flipY = false;
  tex.colorSpace = THREE.SRGBColorSpace;
  // (ops: the recolors done to it, in order - the pack carries them, not the texture; history: {img, ops} to go back to)
  const t = {name: key, canvas, ctx, original, tex, history: [], ops: [], modified: false, format: dds.format, mips: dds.mips,
    id: [info.texture.lo, info.texture.hi]};
  state.textures.set(key, t);
  return t;
}

let selecting = 0;   // (the latest pick wins: one still loading stops when another comes)
async function selectOutfit(o) {
  const mine = ++selecting;
  state.current = o;
  if (state.packModel && state.packModel.character !== o.character) { state.packModel = null; showPackModel(); }   // (another character's)
  for (const c of document.querySelectorAll('.chip')) c.classList.toggle('on', c === o.chip);
  for (const b of document.querySelectorAll('.item')) b.classList.toggle('on', b.dataset.char === o.character);
  setStatus('Loading ' + (NICE[o.character] || o.character) + '...');
  const pieces = await loadModel(state.ho, o.model);
  if (mine !== selecting) return;
  if (state.mode === 'models') {   // (the character to fit a new model on)
    const skel = await loadSkeleton(state.ho, o.model);
    if (!skel) { setStatus('No skeleton found for this outfit.'); return; }
    await models.build(o, pieces, skel);
    frame();
    setStatus('');
    return;
  }
  if (state.mode === 'emotes') {   // (the rigged character, to pose)
    const skel = await loadSkeleton(state.ho, o.model);
    if (!skel) { setStatus('No skeleton found for this outfit.'); return; }
    await emotes.build(o, pieces, skel);
    frame();
    if (!$('emoteName').value) $('emoteName').placeholder = 'My ' + (NICE[o.character] || o.character) + ' emote';
    setStatus('');
    return;
  }
  holder.clear();
  const texs = [];
  for (const p of pieces) {
    const g = new THREE.BufferGeometry();
    g.setAttribute('position', new THREE.BufferAttribute(p.position, 3));
    g.setAttribute('normal', new THREE.BufferAttribute(p.normal, 3));
    g.setAttribute('uv', new THREE.BufferAttribute(p.uv, 2));
    g.setIndex(new THREE.BufferAttribute(p.index, 1));
    const t = p.texture ? await textureFor(p.texture) : null;
    if (mine !== selecting) return;
    if (t && !texs.includes(t)) texs.push(t);
    const m = new THREE.MeshLambertMaterial({map: t ? t.tex : null, side: THREE.DoubleSide, alphaTest: 0.3});
    const mesh = new THREE.Mesh(g, m);
    mesh.userData.texture = t;
    mesh.userData.piece = p;   // (its triangles under a skin's model: hidden while that shows)
    mesh.userData.damage = /damage|dstr/i.test(p.texture?.name || '');   // (battle damage: shown when hurt in the game)
    mesh.visible = !mesh.userData.damage || $('showDamage').checked;
    holder.add(mesh);
  }
  // its picture in the game's skin pickers - a texture of its own here: the recolors go to it too, and the pack keeps
  // what was done to it (the game makes the skin's picture from its own, as it does the textures)
  const pic = await pictureFor(state.ho, o.character, o.outfit).catch(() => null);
  if (mine !== selecting) return;
  if (pic) {
    const t = await textureFor(pic);
    t.picture = true;
    if (!texs.includes(t)) texs.push(t);
  }
  await showPackModel3D();
  frame();
  o.textures = texs;
  showTextures(texs);
  $('savePack').disabled = false;
  if (!$('packName').value) $('packName').placeholder = 'My ' + (NICE[o.character] || o.character) + ' skin';
  setStatus('');
}

$('showDamage').onchange = () => {
  for (const m of holder.children) if (m.userData.damage) m.visible = $('showDamage').checked;
};

function frame() {
  const box = new THREE.Box3().setFromObject(holder);
  const c = box.getCenter(new THREE.Vector3()), dims = box.getSize(new THREE.Vector3()), size = dims.length();
  controls.target.copy(c);
  // (far enough for its height and its width - a tall, narrow view needs more room)
  const tan = Math.tan(THREE.MathUtils.degToRad(camera.fov) / 2);
  const dist = Math.max(dims.y / (2 * tan), Math.max(dims.x, dims.z) / (2 * tan * camera.aspect)) * 1.25 + dims.z / 2;
  camera.position.set(c.x, c.y + size * 0.08, c.z + dist);
  camera.near = size / 100; camera.far = size * 20;
  camera.updateProjectionMatrix();
}

// ---- textures --------------------------------------------------------------------------------------------------
function showTextures(texs) {
  const box = $('texList');
  box.innerHTML = '';
  for (const t of texs) {
    const d = document.createElement('div');
    d.className = 'tex';
    d.title = t.name;
    const c = document.createElement('canvas');
    c.width = 96; c.height = 96;
    d.append(c);
    t.thumb = c; t.thumbBox = d;
    if (t.picture) {   // (the outfit's picture in the game's skin pickers)
      d.title = t.name + ' - its picture in the game\'s skin menus';
      const tag = document.createElement('span');
      tag.className = 'pictag'; tag.textContent = 'PICTURE';
      d.append(tag);
    }
    d.onclick = () => selectTexture(t);
    box.append(d);
    drawThumb(t);
  }
  const main = texs.slice().sort((a, b) => b.canvas.width * b.canvas.height - a.canvas.width * a.canvas.height)[0];
  if (main) selectTexture(main);
}

function drawThumb(t) {
  const x = t.thumb.getContext('2d');
  x.clearRect(0, 0, 96, 96);
  x.drawImage(t.canvas, 0, 0, 96, 96);
  t.thumbBox.querySelector('.mod')?.remove();
  if (t.modified) {
    const m = document.createElement('span');
    m.className = 'mod'; m.textContent = 'EDITED';
    t.thumbBox.append(m);
  }
}

function selectTexture(t) {
  state.selected = t;
  for (const d of document.querySelectorAll('.tex')) d.classList.toggle('on', d === t.thumbBox);
  const big = $('bigTex');
  big.width = t.canvas.width; big.height = t.canvas.height;
  drawBig();
  for (const id of ['exportPng', 'reset', 'undo']) $(id).disabled = false;
  $('undo').disabled = !t.history.length;
  $('reset').disabled = !t.modified;
}

function drawBig() {
  const t = state.selected, big = $('bigTex'), x = big.getContext('2d');
  x.clearRect(0, 0, big.width, big.height);
  x.drawImage(t.canvas, 0, 0);
}

function changed(t) {
  t.tex.needsUpdate = true;
  drawThumb(t);
  if (t === state.selected) { drawBig(); $('undo').disabled = !t.history.length; $('reset').disabled = !t.modified; }
}

function pick(t, px, py) {
  const p = t.ctx.getImageData(Math.max(0, Math.min(t.canvas.width - 1, px)), Math.max(0, Math.min(t.canvas.height - 1, py)), 1, 1).data;
  state.picked = {t, rgb: [p[0], p[1], p[2]]};
  $('fromColor').style.background = `rgb(${p[0]},${p[1]},${p[2]})`;
  $('pickInfo').textContent = 'Picked a color on ' + t.name + ' - choose the new one and Recolor.';
  $('apply').disabled = false;
  if (state.selected !== t) selectTexture(t);
}

$('bigTex').onclick = e => {
  const t = state.selected;
  if (!t) return;
  const r = e.target.getBoundingClientRect();
  pick(t, Math.floor((e.clientX - r.left) / r.width * t.canvas.width), Math.floor((e.clientY - r.top) / r.height * t.canvas.height));
};

// clicking the character: the texture spot under the pointer
const ray = new THREE.Raycaster();
let downAt = null;
renderer.domElement.addEventListener('pointerdown', e => { downAt = [e.clientX, e.clientY]; });
renderer.domElement.addEventListener('pointerup', e => {
  if (!downAt || Math.hypot(e.clientX - downAt[0], e.clientY - downAt[1]) > 4) return;   // (a drag turns the view)
  if (state.mode === 'emotes') { emotes.click(e); return; }
  const r = renderer.domElement.getBoundingClientRect();
  ray.setFromCamera(new THREE.Vector2((e.clientX - r.left) / r.width * 2 - 1, -(e.clientY - r.top) / r.height * 2 + 1), camera);
  const hit = ray.intersectObjects(holder.children.filter(c => !c.userData.packModel)).find(h => !h.object.userData.lookLine);
  const t = hit?.object.userData.texture;
  if (!t || !hit.uv) return;
  const u = ((hit.uv.x % 1) + 1) % 1, v = ((hit.uv.y % 1) + 1) % 1;
  pick(t, Math.floor(u * t.canvas.width), Math.floor(v * t.canvas.height));
});

// Recolor: every pixel close to the picked color moves to the new one - its own light and shade kept (the new color's
// hue, its saturation and lightness shifted by as much as the two colors differ)
function rgb2hsl([r, g, b]) {
  r /= 255; g /= 255; b /= 255;
  const mx = Math.max(r, g, b), mn = Math.min(r, g, b), l = (mx + mn) / 2;
  if (mx === mn) return [0, 0, l];
  const d = mx - mn, s = l > 0.5 ? d / (2 - mx - mn) : d / (mx + mn);
  const h = mx === r ? (g - b) / d + (g < b ? 6 : 0) : mx === g ? (b - r) / d + 2 : (r - g) / d + 4;
  return [h / 6, s, l];
}
function hsl2rgb([h, s, l]) {
  if (!s) return [l * 255, l * 255, l * 255];
  const q = l < 0.5 ? l * (1 + s) : l + s - l * s, p = 2 * l - q;
  const f = t => { t = (t + 1) % 1; return t < 1 / 6 ? p + (q - p) * 6 * t : t < 1 / 2 ? q : t < 2 / 3 ? p + (q - p) * (2 / 3 - t) * 6 : p; };
  return [f(h + 1 / 3) * 255, f(h) * 255, f(h - 1 / 3) * 255];
}

// pixels (rgba) near color P moved to Q, shading kept; returns how many. The game does the same to its own texture
// (src/loader/skinbuild.cpp Recolor) - keep the two alike.
function recolorData(d, P, Q, tol) {
  const hp = rgb2hsl(P), hq = rgb2hsl(Q);
  let count = 0;
  for (let i = 0; i < d.length; i += 4) {
    const dist = Math.hypot(d[i] - P[0], d[i + 1] - P[1], d[i + 2] - P[2]);
    if (dist > tol) continue;
    const w = dist < tol * 0.6 ? 1 : 1 - (dist - tol * 0.6) / (tol * 0.4);   // (soft edge)
    const hc = rgb2hsl([d[i], d[i + 1], d[i + 2]]);
    // (saturation and lightness shifted by as much as the two colors differ - a scaled saturation made a near-white
    //  pick turn every slightly tinted pixel near it fully saturated: lime specks along a recolored white shirt's seams)
    const out = hsl2rgb([hq[0], Math.max(0, Math.min(1, hc[1] + hq[1] - hp[1])), Math.max(0, Math.min(1, hc[2] + hq[2] - hp[2]))]);
    for (let k = 0; k < 3; k++) d[i + k] = d[i + k] * (1 - w) + out[k] * w;
    count++;
  }
  return count;
}

// one texture: recolorData, remembered (its ops) - and undoable
function recolor(t, P, Q, tol) {
  const img = t.ctx.getImageData(0, 0, t.canvas.width, t.canvas.height);
  const count = recolorData(img.data, P, Q, tol);
  if (!count) return 0;
  remember(t);
  t.ops.push([...P, ...Q, tol]);
  t.ctx.putImageData(img, 0, 0);
  t.modified = true;
  changed(t);
  return count;
}

// The outfit's battle damage textures carry their own copy of its colors (torn bits of the clothes, drawn over the
// character in the game): a recolor goes to them too - or the old color shows through in the game.
const isDamage = t => /damage|dstr/i.test(t.name);
// a texture's own color nearest to `rgb` - one of its common ones (over 0.2% of it), within 110 - or null
function nearestColor(t, rgb) {
  const d = t.ctx.getImageData(0, 0, t.canvas.width, t.canvas.height).data, buckets = new Map();
  for (let i = 0; i < d.length; i += 4) {
    if (d[i + 3] < 128) continue;
    const k = (d[i] >> 3) << 10 | (d[i + 1] >> 3) << 5 | d[i + 2] >> 3, b = buckets.get(k) || [0, 0, 0, 0];
    b[0] += d[i]; b[1] += d[i + 1]; b[2] += d[i + 2]; b[3]++;
    buckets.set(k, b);
  }
  const min = d.length / 4 * 0.002;
  let best = null, dist = 110;
  for (const b of buckets.values()) {
    if (b[3] < min) continue;
    const c = [b[0] / b[3], b[1] / b[3], b[2] / b[3]], e = Math.hypot(c[0] - rgb[0], c[1] - rgb[1], c[2] - rgb[2]);
    if (e < dist) { dist = e; best = c.map(Math.round); }
  }
  return best;
}
$('apply').onclick = () => {
  const P = state.picked;
  if (!P) return;
  const hex = $('toColor').value, Q = [1, 3, 5].map(i => parseInt(hex.slice(i, i + 2), 16));
  const tol = +$('tolerance').value * 2.2;
  const count = recolor(P.t, P.rgb, Q, tol);
  let extra = 0;
  if (count) for (const t of state.current?.textures || []) {   // (and to its picture in the skin menus - drawn in its
    if (t === P.t) continue;                                        //  own shades: the nearest of its colors is that one)
    if (isDamage(t)) extra += recolor(t, P.rgb, Q, tol);
    else if (t.picture && !P.t.picture) {
      const near = nearestColor(t, P.rgb);
      if (near) extra += recolor(t, near, Q, tol);
    }
  }
  state.picked = {t: P.t, rgb: Q};
  $('fromColor').style.background = hex;
  setStatus(count ? `Recolored ${count.toLocaleString()} pixels${extra ? ` (and ${extra.toLocaleString()} in the battle damage and the picture)` : ''}.`
    : 'Nothing that close - pick again or allow more with "How close".');
};

// (what to go back to: the pixels and how many recolors there were)
function remember(t) {
  t.history.push({img: t.ctx.getImageData(0, 0, t.canvas.width, t.canvas.height), ops: t.ops.length});
  if (t.history.length > 20) t.history.shift();
}

$('undo').onclick = () => {
  const t = state.selected;
  if (!t?.history.length) return;
  const h = t.history.pop();
  t.ctx.putImageData(h.img, 0, 0);
  t.ops.length = Math.min(t.ops.length, h.ops);
  t.modified = t.history.length > 0 || t.modified;
  changed(t);
};
$('reset').onclick = () => {
  const t = state.selected;
  if (!t) return;
  remember(t);
  t.ops = [];
  t.ctx.putImageData(t.original, 0, 0);
  t.modified = false;
  changed(t);
};

// ---- PNG in / out ----------------------------------------------------------------------------------------------
function download(blob, name) {
  const a = document.createElement('a');
  a.href = URL.createObjectURL(blob);
  a.download = name;
  a.click();
  setTimeout(() => URL.revokeObjectURL(a.href), 5000);
}
const pngOf = t => new Promise(r => t.canvas.toBlob(r, 'image/png'));
$('exportPng').onclick = async () => state.selected && download(await pngOf(state.selected), state.selected.name + '.png');

async function imageInto(t, blob) {
  const img = await createImageBitmap(blob);
  remember(t);
  t.ctx.clearRect(0, 0, t.canvas.width, t.canvas.height);
  t.ctx.drawImage(img, 0, 0, t.canvas.width, t.canvas.height);
  t.modified = true;
  changed(t);
  return img;
}
$('importPng').onchange = async e => {
  const f = e.target.files[0], t = state.selected;
  e.target.value = '';
  if (!f || !t) return;
  const img = await imageInto(t, f);
  setStatus(img.width === t.canvas.width && img.height === t.canvas.height ? 'Imported.' :
    `Imported (resized from ${img.width}x${img.height} to ${t.canvas.width}x${t.canvas.height}).`);
};

// ---- skin packs ------------------------------------------------------------------------------------------------
// The pack (format 2): skin.json - for each changed texture its recolors ("ops": [from r, g, b, to r, g, b, how close],
// in order) and, if anything else was changed (painted, imported), an overlay PNG holding only those pixels (the rest
// transparent). No game art: the game makes the texture from the player's own copy (src/loader/skinbuild.cpp).
const pngOfCanvas = c => new Promise(r => c.toBlob(r, 'image/png'));
function overlayOf(t) {   // the pixels that aren't the recolors' doing - or null
  const w = t.canvas.width, h = t.canvas.height;
  const base = new Uint8ClampedArray(t.original.data);
  for (const [r0, g0, b0, r1, g1, b1, tol] of t.ops) recolorData(base, [r0, g0, b0], [r1, g1, b1], tol);
  const now = t.ctx.getImageData(0, 0, w, h).data, out = new ImageData(w, h);
  let n = 0;
  for (let i = 0; i < now.length; i += 4) {
    if (now[i + 3] === 0) continue;
    if (Math.abs(now[i] - base[i]) + Math.abs(now[i + 1] - base[i + 1]) + Math.abs(now[i + 2] - base[i + 2]) <= 6) continue;
    out.data.set([now[i], now[i + 1], now[i + 2], 255], i);
    n++;
  }
  if (!n) return null;
  const c = document.createElement('canvas');
  c.width = w; c.height = h;
  c.getContext('2d').putImageData(out, 0, 0);
  return {canvas: c, pixels: n};
}

// A model pack can come with the skin (outfit pieces made in the Models tab, worn over the character): stored in the
// pack as model.fgmodel - the game puts it on with the skin, and online it goes along with it.
// the skin's model on the character (Skins tab) - as it stands; clicking picks the character's colors through it
async function showPackModel3D() {
  for (const c of holder.children.filter(x => x.userData.packModel)) holder.remove(c);
  const m = state.packModel;
  const on = state.mode === 'skins' && m && $('packModelShow').checked && m.character === state.current?.character;
  if (on && !m.group) Object.assign(m, await modelPackGroup(m.bytes));
  for (const mesh of holder.children) {   // (the character's triangles under it, as the game leaves them out)
    const p = mesh.userData.piece;
    if (!p) continue;
    const h = on && (m.info.hide || []).find(x => x.verts === p.position.length / 3 && x.indices === p.index.length);
    let index = p.index;
    if (h) {
      index = p.index.slice();
      for (let r = 0; r + 1 < h.tris.length; r += 2)
        for (let k = h.tris[r]; k < h.tris[r] + h.tris[r + 1]; k++) index[k * 3 + 1] = index[k * 3 + 2] = index[k * 3];
    }
    mesh.geometry.setIndex(new THREE.BufferAttribute(index, 1));
  }
  if (!on) return;
  m.group.userData.packModel = true;
  holder.add(m.group);
}
$('packModelShow').onchange = () => showPackModel3D();

function showPackModel() {
  const m = state.packModel;
  $('packModelShowRow').hidden = !m;
  showPackModel3D();
  $('packModelInfo').textContent = m ? `With its model: "${m.name}" (${Math.round(m.bytes.length / 1024).toLocaleString()} KB).`
    : 'Outfit pieces made in the Models tab (a hat, a vest...) can come with the skin.';
  $('packModelOff').hidden = !m;
}
function modelPackInfo(bytes) {   // its model.json (a model pack - or an error)
  const files = unzipSync(bytes);
  if (!files['model.json'] || !files['mesh.bin']) throw new Error("it isn't a model pack");
  return JSON.parse(strFromU8(files['model.json']));
}
$('packModel').onchange = async e => {
  const f = e.target.files[0];
  e.target.value = '';
  if (!f) return;
  try {
    const bytes = new Uint8Array(await f.arrayBuffer()), info = modelPackInfo(bytes);
    if (state.current && info.character !== state.current.character)
      throw new Error(`it's made for ${NICE[info.character] || info.character}, this skin is for ${NICE[state.current.character] || state.current.character}`);
    state.packModel = {name: info.name || f.name, bytes, character: info.character};
    showPackModel();
    setStatus(`"${state.packModel.name}" comes with this skin${info.over ? '' : ' (it takes the character\'s place)'}.`);
  } catch (err) {
    setStatus("Couldn't use that model: " + err.message);
  }
};
$('packModelOff').onclick = () => { state.packModel = null; showPackModel(); };

// The pack's pictures (the game's menus show them - its loader's icons.cpp): icon.png, its tile on the character screen
// (head to belt), and head.png, the HUD's head - drawn flat like the game's own 2D HUD art: each part its plain colors
// (no light or shade), a bold black outline, a palette of a few colors (kept small: the game draws the picture as a
// shape, a border per color change), turned a little, transparent around. Its eyes open: the view is the rest pose,
// whose lids are shut (the game opens them) - what its face bones move (lids, brows) is left out of the pictures.
const ICON_LINE = new THREE.MeshBasicMaterial({color: 0x141414, side: THREE.BackSide});
ICON_LINE.onBeforeCompile = sh => {
  sh.vertexShader = sh.vertexShader.replace('#include <skinning_vertex>', `#include <skinning_vertex>
  #ifdef USE_SKINNING
    transformed += normalize(objectNormal) * 0.008;
  #else
    transformed += normalize(normal) * 0.008;
  #endif`);
};
// Where the face is in a head picture (fractions of it: its left and right at the cheeks - 62% down - and its bottom):
// the game sizes it so the face is as wide as its own character's head, the hat standing out of it.
function faceOf(c) {
  const W = c.width, H = c.height, d = c.getContext('2d').getImageData(0, 0, W, H).data, a = (x, y) => d[(y * W + x) * 4 + 3] > 0;
  let bottom = H - 1;
  while (bottom > 0 && ![...Array(W).keys()].some(x => a(x, bottom))) bottom--;
  let top = 0;
  while (top < bottom && ![...Array(W).keys()].some(x => a(x, top))) top++;
  const y = Math.round(top + (bottom - top) * 0.62);
  let l = 0, r = W - 1;
  while (l < r && !a(l, y)) l++;
  while (r > l && !a(r, y)) r--;
  return [+(l / W).toFixed(4), +((r + 1) / W).toFixed(4), +((bottom + 1) / H).toFixed(4)];
}
// A head picture rounded off under its face, as the game's heads are (not a cut through the shoulders): below the
// cheeks only what's within a half-ellipse as wide as the face, a black line along that edge.
function chin(c) {
  const [l, r, b] = faceOf(c), W = c.width, H = c.height, ctx = c.getContext('2d');
  const d = ctx.getImageData(0, 0, W, H).data;
  let top = 0;
  while (top < H - 1 && ![...Array(W).keys()].some(x => d[(top * W + x) * 4 + 3] > 0)) top++;
  const y0 = top + (b * H - top) * 0.62, cx = (l + r) / 2 * W, ax = (r - l) / 2 * W * 1.03, ay = (b * H - y0) * 1.02;
  const edge = () => { ctx.beginPath(); ctx.moveTo(0, 0); ctx.lineTo(W, 0); ctx.lineTo(W, y0); ctx.lineTo(cx + ax, y0);
    ctx.ellipse(cx, y0, ax, ay, 0, 0, Math.PI); ctx.lineTo(0, y0); ctx.closePath(); };
  ctx.save();
  ctx.globalCompositeOperation = 'destination-in';
  edge();
  ctx.fill();
  ctx.globalCompositeOperation = 'source-atop';
  ctx.beginPath();
  ctx.ellipse(cx, y0, ax - 1, ay - 1, 0, 0, Math.PI);
  ctx.strokeStyle = '#141414';
  ctx.lineWidth = Math.max(2, W / 50);
  ctx.stroke();
  ctx.restore();
  return c;
}
function flatColors(c, n = 28) {   // (the picture in n colors - the most used, near ones merged - and no half-transparency)
  const ctx = c.getContext('2d'), img = ctx.getImageData(0, 0, c.width, c.height), d = img.data, buckets = new Map();
  for (let i = 0; i < d.length; i += 4) {
    if (d[i + 3] < 128) continue;
    const k = (d[i] >> 4) << 8 | (d[i + 1] >> 4) << 4 | d[i + 2] >> 4, b = buckets.get(k) || [0, 0, 0, 0];
    b[0] += d[i]; b[1] += d[i + 1]; b[2] += d[i + 2]; b[3]++;
    buckets.set(k, b);
  }
  const pal = [[255, 255, 255], [20, 20, 20]].concat(   // (white and black always: eyes, glasses, lines)
    [...buckets.values()].sort((x, y) => y[3] - x[3]).slice(0, n).map(b => [b[0] / b[3], b[1] / b[3], b[2] / b[3]]));
  for (let i = 0; i < d.length; i += 4) {
    if (d[i + 3] < 128) { d[i + 3] = 0; continue; }
    let best = 0, dist = Infinity;
    pal.forEach((q, j) => { const e = (d[i] - q[0]) ** 2 + (d[i + 1] - q[1]) ** 2 + (d[i + 2] - q[2]) ** 2; if (e < dist) { dist = e; best = j; } });
    d[i] = pal[best][0]; d[i + 1] = pal[best][1]; d[i + 2] = pal[best][2]; d[i + 3] = 255;
  }
  ctx.putImageData(img, 0, 0);
  return c;
}
async function renderIcons(scale = 1) {   // (scale: bigger, to look at)
  const was = {look: $('gameLook').checked, model: $('packModelShow').checked};
  $('gameLook').checked = false;   // (its own materials back first)
  syncLook();
  if (state.packModel) { $('packModelShow').checked = true; await showPackModel3D(); }
  const hidden = holder.children.filter(m => m.userData.damage && m.visible);
  for (const m of hidden) m.visible = false;
  const skel = state.current ? await loadSkeleton(state.ho, state.current.model).catch(() => null) : null;
  const face = new Set((skel?.names || []).map((n, i) => /^Face /.test(n) ? i : -1).filter(i => i >= 0));
  const opened = [];
  for (const o of holder.children) {   // (the face bones' triangles collapsed for now)
    const p = o.userData.piece;
    if (!p?.skin || !face.size || !o.geometry.index) continue;
    const idx = o.geometry.index.array, was = idx.slice(), w = p.skin.weight, b = p.skin.index;
    const moved = v => [0, 1, 2, 3].some(k => w[v * 4 + k] > 0.3 && face.has(b[v * 4 + k]));
    for (let t = 0; t + 2 < idx.length; t += 3) if (moved(idx[t]) || moved(idx[t + 1]) || moved(idx[t + 2])) idx[t + 1] = idx[t + 2] = idx[t];
    o.geometry.index.needsUpdate = true;
    opened.push([o, was]);
  }
  const swapped = [], lines = [];
  holder.traverse(o => {
    if (!o.isMesh || !o.visible) return;
    const m = o.material;
    swapped.push([o, m]);
    o.material = new THREE.MeshBasicMaterial({map: m.map, color: m.color, side: m.side, alphaTest: m.alphaTest});
    if (o.userData.piece || partSize(o) >= 0.07) lines.push(o);
  });
  for (const o of lines) {
    const line = o.isSkinnedMesh ? new THREE.SkinnedMesh(o.geometry, ICON_LINE) : new THREE.Mesh(o.geometry, ICON_LINE);
    if (o.isSkinnedMesh) line.bind(o.skeleton, o.bindMatrix);
    line.userData.lookLine = line.userData.iconLine = true;
    line.frustumCulled = false;
    o.add(line);
  }
  holder.updateMatrixWorld(true);
  const all = new THREE.Box3(), body = new THREE.Box3();
  holder.traverse(o => {
    if (!o.isMesh || o.userData.lookLine || !o.visible) return;
    all.expandByObject(o);
    if (o.userData.piece) body.expandByObject(o);
  });
  const H = body.max.y - body.min.y, cx = (body.min.x + body.max.x) / 2, cz = (body.min.z + body.max.z) / 2;
  const top = Math.max(all.max.y, body.max.y) + H * 0.02;
  const ratio = renderer.getPixelRatio();
  renderer.setPixelRatio(1);
  const shoot = (w, h, y0, cut) => {   // (from y0 up to the top, w x h pixels - from a little below: the eyes under a
    const half = (top - y0) / 2, cy = (top + y0) / 2, turn = THREE.MathUtils.degToRad(-16), up = THREE.MathUtils.degToRad(-11);
    const cam = new THREE.OrthographicCamera(-half * w / h, half * w / h, half, -half, 0.01, 50);
    cam.position.set(cx + Math.sin(turn) * Math.cos(up) * 10, cy + Math.sin(up) * 10, cz + Math.cos(turn) * Math.cos(up) * 10);
    cam.lookAt(cx, cy, cz);
    renderer.setSize(w, h, false);   //  brim; cut: nothing below that)
    renderer.clippingPlanes = cut === undefined ? [] : [new THREE.Plane(new THREE.Vector3(0, 1, 0), -cut)];
    renderer.render(scene, cam);
    renderer.clippingPlanes = [];
    const c = document.createElement('canvas');
    c.width = w; c.height = h;
    c.getContext('2d').drawImage(renderer.domElement, 0, 0, w, h);
    return flatColors(c);
  };
  // (the head: cut under the chin; the tile: head to belt, edge to edge)
  const head = chin(shoot(128 * scale, 128 * scale, body.max.y - H * 0.33, body.max.y - H * 0.3));
  const icon = shoot(152 * scale, 212 * scale, body.max.y - H * 0.6);
  renderer.setPixelRatio(ratio);
  for (const o of lines) for (const c of o.children.filter(c => c.userData.iconLine)) o.remove(c);
  for (const [o, m] of swapped) { o.material.dispose(); o.material = m; }
  for (const m of hidden) m.visible = true;
  for (const [o, idx] of opened) { o.geometry.index.array.set(idx); o.geometry.index.needsUpdate = true; }
  $('gameLook').checked = was.look;
  $('packModelShow').checked = was.model;
  if (state.packModel) await showPackModel3D();
  syncLook();
  resize();
  const png = async c => new Uint8Array(await (await pngOfCanvas(c)).arrayBuffer());
  return {icon: await png(icon), head: await png(head), face: faceOf(head)};
}

// The skin's own picture (Skins tab "Skin picture": a 2D drawing, like the game's skin art): its tile in the menus is
// the whole picture, fitted (head to toe, transparent round it); the HUD's head the top of it - as wide as the widest
// part of its top 30% (a hat's brim) deep, all of those rows across. Both in a few flat colors
// (the game draws a picture as a shape: a border per color change).
async function pictureIcons(img) {
  const W = img.width, H = img.height, src = document.createElement('canvas');
  src.width = W; src.height = H;
  const sx = src.getContext('2d');
  sx.drawImage(img, 0, 0);
  const d = sx.getImageData(0, 0, W, H).data, solid = (x, y) => d[(y * W + x) * 4 + 3] > 64;
  let top = 0, bottom = H - 1, x0 = W, x1 = -1;
  while (top < H - 1 && ![...Array(W).keys()].some(x => solid(x, top))) top++;
  while (bottom > top && ![...Array(W).keys()].some(x => solid(x, bottom))) bottom--;
  const band = top + Math.round((bottom - top) * 0.3);
  for (let y = top; y <= band; y++) for (let x = 0; x < W; x++) if (solid(x, y)) { x0 = Math.min(x0, x); x1 = Math.max(x1, x); }
  const icon = document.createElement('canvas');   // (152 x 212, the picture contained, centred)
  icon.width = 152; icon.height = 212;
  const s = Math.min(152 / W, 212 / (bottom - top + 1));
  icon.getContext('2d').drawImage(src, 0, top, W, bottom - top + 1, (152 - W * s) / 2, (212 - (bottom - top + 1) * s) / 2, W * s, (bottom - top + 1) * s);
  const side = Math.min(Math.max(8, x1 - x0 + 1), bottom - top + 1), head = document.createElement('canvas'), pad = 5;
  for (let y = top; y < top + side; y++) for (let x = 0; x < W; x++) if (solid(x, y)) { x0 = Math.min(x0, x); x1 = Math.max(x1, x); }
  const cw = x1 - x0 + 1, k = Math.min((128 - 2 * pad) / cw, (128 - 2 * pad) / side);   // (all of those rows, fitted)
  head.width = head.height = 128;
  head.getContext('2d').drawImage(src, x0, top, cw, side, (128 - cw * k) / 2, 128 - pad - side * k, cw * k, side * k);
  flatColors(icon, 40);
  chin(flatColors(head, 40));
  const png = async c => new Uint8Array(await (await pngOfCanvas(c)).arrayBuffer());
  return {icon: await png(icon), head: await png(head), face: faceOf(head)};
}
function showPackPicture() {
  const p = state.packPicture;
  $('packPictureShow').hidden = !p;
  if (p) $('packPictureShow').src = p.url;
  $('packPictureOff').hidden = !p;
}
$('packPicture').onchange = async e => {
  const f = e.target.files[0];
  e.target.value = '';
  if (!f) return;
  try {
    const img = await createImageBitmap(f);
    state.packPicture = {img, url: URL.createObjectURL(f)};
    showPackPicture();
    setStatus(`"${f.name}" is this skin's picture in the game's menus.`);
  } catch (err) {
    setStatus("Couldn't use that picture: " + err.message);
  }
};
$('packPictureOff').onclick = () => { state.packPicture = null; showPackPicture(); };

async function buildPack() {
  const o = state.current;
  const edited = (o?.textures || []).filter(t => t.modified);
  if (!o || (!edited.length && !state.packModel)) return null;
  const name = ($('packName').value || $('packName').placeholder).trim();
  const files = {};
  const manifest = {format: 2, name, character: o.character, outfit: o.outfit, model: o.model.name,
    modelId: [o.model.lo, o.model.hi], textures: {}, tool: 'FGUY Skin Studio'};
  if (state.packModel) {
    files['model.fgmodel'] = [state.packModel.bytes, {level: 0}];   // (stored, as the mod reads it)
    manifest.withModel = state.packModel.name;
  }
  for (const t of edited) {
    const entry = {id: t.id, width: t.canvas.width, height: t.canvas.height, format: t.format, mips: t.mips, ops: t.ops};
    if (t.picture) entry.picture = true;   // (the skin's picture in the game's menus: made from the game's own there)
    const ov = overlayOf(t);
    if (ov) {
      entry.overlay = 'textures/' + t.name + '.png';
      files[entry.overlay] = [new Uint8Array(await (await pngOfCanvas(ov.canvas)).arrayBuffer()), {level: 0}];   // (stored)
    }
    manifest.textures[t.name] = entry;
  }
  // its pictures in the game's menus: a drawing of its own ("Skin picture"); else the outfit's own picture, as changed
  // here (the game makes it - nothing to store but the changes); else drawn from the view
  const ownPicture = (o.textures || []).some(t => t.picture);
  if (state.packPicture || !ownPicture) {
    const pics = state.packPicture ? await pictureIcons(state.packPicture.img) : await renderIcons();
    if (state.packPicture) manifest.picture = 'icon.png';   // (made from a picture: opened again, its tile is that)
    files['icon.png'] = [pics.icon, {level: 0}];   // (stored)
    files['head.png'] = [pics.head, {level: 0}];
    manifest.icon = 'icon.png';
    manifest.head = 'head.png';
    manifest.headFace = pics.face;   // (the face in it: the game sizes the head by it)
  }
  files['skin.json'] = [strToU8(JSON.stringify(manifest, null, 2)), {level: 0}];   // (stored: the mod reads it as is)
  return {name, count: edited.length, blob: new Blob([zipSync(files)])};
}
state.buildPack = buildPack;
state.renderIcons = renderIcons;
$('savePack').onclick = async () => {
  const p = await buildPack();
  if (!p) { setStatus('Change something first - nothing to save yet.'); return; }
  download(p.blob, p.name.replace(/[^\w\- ]+/g, '').trim().replace(/ +/g, '_') + '.fgskin');
  setStatus('Saved "' + p.name + '" (' + p.count + ' texture' + (p.count === 1 ? '' : 's') +
    (state.packModel ? ', with its model' : '') + ').');
};

$('openPack').onchange = async e => {
  const f = e.target.files[0];
  e.target.value = '';
  if (f) await openPackBytes(new Uint8Array(await f.arrayBuffer()), f.name);
};
async function openPackBytes(bytes, label) {
  if (!state.ho) { setStatus('Open your game folder first.'); return; }
  try {
    const files = unzipSync(bytes);
    const m = JSON.parse(strFromU8(files['skin.json']));
    const o = state.outfits.find(x => x.model.name === m.model);
    if (!o) throw new Error('its outfit (' + m.model + ") isn't in this game");
    await selectOutfit(o);
    for (const [name, info] of Object.entries(m.textures)) {
      const t = o.textures.find(x => x.name === name);
      if (!t) continue;
      if (m.format >= 2) {   // (its recolors done again on the game's texture, then its own pixels on top)
        remember(t);
        const img = new ImageData(new Uint8ClampedArray(t.original.data), t.canvas.width, t.canvas.height);
        for (const [r0, g0, b0, r1, g1, b1, tol] of info.ops || []) recolorData(img.data, [r0, g0, b0], [r1, g1, b1], tol);
        t.ctx.putImageData(img, 0, 0);
        t.ops = (info.ops || []).map(x => x.slice());
        if (info.overlay && files[info.overlay]) t.ctx.drawImage(await createImageBitmap(new Blob([files[info.overlay]], {type: 'image/png'})), 0, 0);
        t.modified = true;
        changed(t);
      } else if (files[info.file]) {
        await imageInto(t, new Blob([files[info.file]], {type: 'image/png'}));
      }
    }
    $('packName').value = m.name || '';
    state.packModel = files['model.fgmodel'] ? {name: m.withModel || modelPackInfo(files['model.fgmodel']).name,
      bytes: files['model.fgmodel'], character: o.character} : null;
    showPackModel();
    state.packPicture = null;
    if (m.picture && files[m.picture]) {
      const blob = new Blob([files[m.picture]], {type: 'image/png'});
      state.packPicture = {img: await createImageBitmap(blob), url: URL.createObjectURL(blob)};
    }
    showPackPicture();
    state.fromPack = true;
    setStatus('Opened "' + (m.name || label) + '"' + (state.packModel ? ', with its model' : '') + '.');
  } catch (err) {
    setStatus("Couldn't open that pack: " + err.message);
  }
}

function setStatus(s) { for (const id of ['status', 'status2', 'status3']) $(id).textContent = s; }

// ---- Skins / Emotes --------------------------------------------------------------------------------------------
models = createModelEditor({holder, $, setStatus, download, textureFor, outfitsOf: ch => state.outfits.filter(o => o.character === ch)});
emotes = createEmoteEditor({scene, camera, controls, renderer, holder, $, setStatus, download, textureFor,
  outfitsOf: ch => state.outfits.filter(o => o.character === ch)});
function setMode(mode) {
  if (mode === state.mode) return;
  if (state.mode === 'emotes') emotes.leave();
  state.mode = mode;
  $('tabSkins').classList.toggle('on', mode === 'skins');
  $('tabEmotes').classList.toggle('on', mode === 'emotes');
  $('side').hidden = mode !== 'skins';
  $('emoteSide').hidden = mode !== 'emotes';
  $('modelSide').hidden = mode !== 'models';
  $('tabModels').classList.toggle('on', mode === 'models');
  $('timeline').hidden = mode !== 'emotes';
  $('damageOpt').hidden = mode !== 'skins';
  $('hint').textContent = mode === 'models' ? 'Drag to turn - scroll to zoom'
    : mode === 'emotes' ? 'Drag to turn - scroll to zoom - click a joint to pose it'
    : 'Drag to turn - scroll to zoom - click the character to pick a color';
  if (state.current) selectOutfit(state.current);
}
$('tabSkins').onclick = () => setMode('skins');
$('tabEmotes').onclick = () => setMode('emotes');
$('tabModels').onclick = () => setMode('models');
resize();

// (development only: dev_server.py on this PC hands over the game file - index.html?dev)
if (new URLSearchParams(location.search).has('dev') && ['127.0.0.1', 'localhost'].includes(location.hostname)) {
  fetch('/game/PLYS.ho').then(r => r.ok ? r.blob() : null).then(b => b && openPackage(new File([b], 'PLYS.ho')));
}
window.studio = state;   // (for checking in the browser's console)
window.studioView = {holder, camera, controls, setMode, emotes, models, render: () => renderer.render(scene, camera)};
