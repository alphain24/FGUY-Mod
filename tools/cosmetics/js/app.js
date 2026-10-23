// FGUY Skin Studio: pick the game folder, a character and outfit, see it in 3D, recolor / repaint its textures, save a
// skin pack (.fgskin: a zip with skin.json - what was done to each texture, no game art) for the game's cosmetics mod - or, on the
// Emotes tab, pose the character on a timeline and save an emote pack (.fgemote, emotes.js) for the emote mod.
import * as THREE from 'three';
import {OrbitControls} from 'three/addons/controls/OrbitControls.js';
import {zipSync, unzipSync, strToU8, strFromU8} from 'fflate';
import {Ho} from './ho.js';
import {decodeDds} from './dds.js';
import {loadModel, loadSkeleton, outfitsOf, CHARACTERS} from './model.js';
import {createEmoteEditor} from './emotes.js';

const $ = id => document.getElementById(id);
const NICE = {AdamWest: 'Adam West'};
const state = {ho: null, outfits: [], current: null, textures: new Map(), selected: null, picked: null, mode: 'skins'};
let emotes = null;   // (the emote editor, made below)

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
renderer.setAnimationLoop(() => {
  const dt = clock.getDelta();
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
  buildCharacterList();
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

function buildCharacterList() {
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
      c.onclick = () => selectOutfit(o);
      o.chip = c;
      row.append(c);
    }
    b.onclick = () => selectOutfit(outfits[0]);
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

async function selectOutfit(o) {
  state.current = o;
  for (const c of document.querySelectorAll('.chip')) c.classList.toggle('on', c === o.chip);
  for (const b of document.querySelectorAll('.item')) b.classList.toggle('on', b.dataset.char === o.character);
  setStatus('Loading ' + (NICE[o.character] || o.character) + '...');
  const pieces = await loadModel(state.ho, o.model);
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
    if (t && !texs.includes(t)) texs.push(t);
    const m = new THREE.MeshLambertMaterial({map: t ? t.tex : null, side: THREE.DoubleSide, alphaTest: 0.3});
    const mesh = new THREE.Mesh(g, m);
    mesh.userData.texture = t;
    mesh.userData.damage = /damage|dstr/i.test(p.texture?.name || '');   // (battle damage: shown when hurt in the game)
    mesh.visible = !mesh.userData.damage || $('showDamage').checked;
    holder.add(mesh);
  }
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
  const hit = ray.intersectObjects(holder.children)[0];
  const t = hit?.object.userData.texture;
  if (!t || !hit.uv) return;
  const u = ((hit.uv.x % 1) + 1) % 1, v = ((hit.uv.y % 1) + 1) % 1;
  pick(t, Math.floor(u * t.canvas.width), Math.floor(v * t.canvas.height));
});

// Recolor: every pixel close to the picked color moves to the new one - its own light and shade kept (hue and
// saturation taken from the new color, its lightness shifted by as much as the two colors differ)
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
    const out = hsl2rgb([hq[0], Math.min(1, hq[1] * (hp[1] > 0.05 ? hc[1] / hp[1] : 1)), Math.max(0, Math.min(1, hc[2] + hq[2] - hp[2]))]);
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
$('apply').onclick = () => {
  const P = state.picked;
  if (!P) return;
  const hex = $('toColor').value, Q = [1, 3, 5].map(i => parseInt(hex.slice(i, i + 2), 16));
  const tol = +$('tolerance').value * 2.2;
  const count = recolor(P.t, P.rgb, Q, tol);
  let extra = 0;
  if (count) for (const t of state.current?.textures || []) if (t !== P.t && isDamage(t)) extra += recolor(t, P.rgb, Q, tol);
  state.picked = {t: P.t, rgb: Q};
  $('fromColor').style.background = hex;
  setStatus(count ? `Recolored ${count.toLocaleString()} pixels${extra ? ` (and ${extra.toLocaleString()} in the battle damage)` : ''}.`
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

async function buildPack() {
  const o = state.current;
  const edited = (o?.textures || []).filter(t => t.modified);
  if (!edited.length) return null;
  const name = ($('packName').value || $('packName').placeholder).trim();
  const files = {};
  const manifest = {format: 2, name, character: o.character, outfit: o.outfit, model: o.model.name,
    modelId: [o.model.lo, o.model.hi], textures: {}, tool: 'FGUY Skin Studio'};
  for (const t of edited) {
    const entry = {id: t.id, width: t.canvas.width, height: t.canvas.height, format: t.format, mips: t.mips, ops: t.ops};
    const ov = overlayOf(t);
    if (ov) {
      entry.overlay = 'textures/' + t.name + '.png';
      files[entry.overlay] = [new Uint8Array(await (await pngOfCanvas(ov.canvas)).arrayBuffer()), {level: 0}];   // (stored)
    }
    manifest.textures[t.name] = entry;
  }
  files['skin.json'] = [strToU8(JSON.stringify(manifest, null, 2)), {level: 0}];   // (stored: the mod reads it as is)
  return {name, count: edited.length, blob: new Blob([zipSync(files)])};
}
state.buildPack = buildPack;
$('savePack').onclick = async () => {
  const p = await buildPack();
  if (!p) { setStatus('Change something first - nothing to save yet.'); return; }
  download(p.blob, p.name.replace(/[^\w\- ]+/g, '').trim().replace(/ +/g, '_') + '.fgskin');
  setStatus('Saved "' + p.name + '" (' + p.count + ' texture' + (p.count > 1 ? 's' : '') + ').');
};

$('openPack').onchange = async e => {
  const f = e.target.files[0];
  e.target.value = '';
  if (!f) return;
  if (!state.ho) { setStatus('Open your game folder first.'); return; }
  try {
    const files = unzipSync(new Uint8Array(await f.arrayBuffer()));
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
    setStatus('Opened "' + (m.name || f.name) + '".');
  } catch (err) {
    setStatus("Couldn't open that pack: " + err.message);
  }
};

function setStatus(s) { $('status').textContent = s; $('status2').textContent = s; }

// ---- Skins / Emotes --------------------------------------------------------------------------------------------
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
  $('timeline').hidden = mode !== 'emotes';
  $('damageOpt').hidden = mode !== 'skins';
  $('hint').textContent = mode === 'emotes' ? 'Drag to turn - scroll to zoom - click a joint to pose it'
    : 'Drag to turn - scroll to zoom - click the character to pick a color';
  if (state.current) selectOutfit(state.current);
}
$('tabSkins').onclick = () => setMode('skins');
$('tabEmotes').onclick = () => setMode('emotes');
resize();

// (development only: dev_server.py on this PC hands over the game file - index.html?dev)
if (new URLSearchParams(location.search).has('dev') && ['127.0.0.1', 'localhost'].includes(location.hostname)) {
  fetch('/game/PLYS.ho').then(r => r.ok ? r.blob() : null).then(b => b && openPackage(new File([b], 'PLYS.ho')));
}
window.studio = state;   // (for checking in the browser's console)
window.studioView = {holder, camera, controls, setMode, emotes, render: () => renderer.render(scene, camera)};
