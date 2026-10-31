// Emotes: pose a character's bones on a timeline (or bring in an animation made elsewhere), save an emote pack
// (.fgemote: a zip with emote.json and motion.bin) that the game's emote mod plays on your player.
// motion.bin: "FGMO", version 1, bones, frames (int32s); a frame: its time (s), then a bone: rotation x y z w,
// translation x y z, scale x y z (floats), in its parent's space. Its bone 0 is the character's root; bone n + 1 the
// skeleton's bone n - as the game's pose (src/loader/motion.cpp).
import * as THREE from 'three';
import {TransformControls} from 'three/addons/controls/TransformControls.js';
import {GLTFLoader} from 'three/addons/loaders/GLTFLoader.js';
import {GLTFExporter} from 'three/addons/exporters/GLTFExporter.js';
import {zipSync, unzipSync, strToU8, strFromU8} from 'fflate';

const FPS = 30;

export function createEmoteEditor({scene, camera, controls, renderer, holder, $, setStatus, download, textureFor, outfitsOf}) {
  const ed = {rig: null, keys: [], time: 0, playing: false, selected: -1, character: null};
  const tc = new TransformControls(camera, renderer.domElement);
  tc.setMode('rotate');
  tc.setSpace('local');
  tc.setSize(0.7);
  tc.addEventListener('dragging-changed', e => {
    controls.enabled = !e.value;
    if (!e.value) { setKey(); }   // (a bone moved: the pose is a key at this time)
  });
  scene.add(tc);

  // ---- the rigged character ----------------------------------------------------------------------------------
  async function build(outfit, pieces, skel) {
    holder.clear();
    tc.detach();
    const root = new THREE.Group();
    const bones = skel.local.map((m, i) => {
      const b = new THREE.Bone();
      b.name = 'bone' + String(i).padStart(2, '0');
      m.decompose(b.position, b.quaternion, b.scale);
      return b;
    });
    bones.forEach((b, i) => (skel.parent[i] < 0 ? root : bones[skel.parent[i]]).add(b));
    holder.add(root);
    root.updateMatrixWorld(true);
    const skeleton = new THREE.Skeleton(bones, skel.invBind.map(m => m.clone()));
    for (const p of pieces) {
      if (!p.skin) continue;
      const g = new THREE.BufferGeometry();
      g.setAttribute('position', new THREE.BufferAttribute(p.position, 3));
      g.setAttribute('normal', new THREE.BufferAttribute(p.normal, 3));
      g.setAttribute('uv', new THREE.BufferAttribute(p.uv, 2));
      g.setAttribute('skinIndex', new THREE.Uint16BufferAttribute(p.skin.index, 4));
      g.setAttribute('skinWeight', new THREE.Float32BufferAttribute(p.skin.weight, 4));
      g.setIndex(new THREE.BufferAttribute(p.index, 1));
      const t = p.texture ? await textureFor(p.texture) : null;
      const mesh = new THREE.SkinnedMesh(g, new THREE.MeshLambertMaterial({map: t ? t.tex : null, side: THREE.DoubleSide, alphaTest: 0.3}));
      mesh.bind(skeleton, new THREE.Matrix4());
      mesh.frustumCulled = false;
      holder.add(mesh);
    }
    // a dot on each joint, to pick it
    const dotGeo = new THREE.SphereGeometry(0.012, 10, 8);
    const dots = bones.map((b, i) => {
      const d = new THREE.Mesh(dotGeo, new THREE.MeshBasicMaterial({color: 0x5cc8ff, depthTest: false, transparent: true, opacity: 0.85}));
      d.renderOrder = 10;
      d.userData.bone = i;
      b.add(d);
      return d;
    });
    const rest = bones.map(b => ({q: b.quaternion.toArray(), p: b.position.toArray()}));
    ed.rig = {outfit, skel, bones, root, dots, rest};
    if (ed.character !== outfit.character) { ed.keys = []; ed.time = 0; }   // (another skeleton: start over)
    ed.character = outfit.character;
    fillBoneList();
    select(-1);
    applyAt(ed.time);
    drawTimeline();
  }

  // ---- poses -------------------------------------------------------------------------------------------------
  const capture = () => ed.rig.bones.map(b => ({q: b.quaternion.toArray(), p: b.position.toArray()}));
  function apply(pose) {
    ed.rig.bones.forEach((b, i) => { b.quaternion.fromArray(pose[i].q); b.position.fromArray(pose[i].p); });
  }
  const qa = new THREE.Quaternion(), qb = new THREE.Quaternion();
  function blend(a, b, k) {
    return a.map((x, i) => {
      qa.fromArray(x.q); qb.fromArray(b[i].q);
      return {q: qa.slerp(qb, k).toArray(), p: x.p.map((v, j) => v + (b[i].p[j] - v) * k)};
    });
  }
  // the pose at time t: between the keys around it (looping: after the last key, back toward the first)
  function poseAt(t) {
    const keys = ed.keys;
    if (!keys.length) return ed.rig.rest;
    if (keys.length === 1) return keys[0].pose;
    const loop = $('emoteLoop').checked, dur = duration();
    let i = keys.findIndex(k => k.t > t);
    if (i === 0) {
      if (!loop) return keys[0].pose;
      const a = keys[keys.length - 1], span = keys[0].t + dur - a.t;
      return blend(a.pose, keys[0].pose, span > 0 ? (t + dur - a.t) / span : 0);
    }
    if (i < 0) {
      const a = keys[keys.length - 1];
      if (!loop) return a.pose;
      const span = keys[0].t + dur - a.t;
      return blend(a.pose, keys[0].pose, span > 0 ? (t - a.t) / span : 0);
    }
    const a = keys[i - 1], b = keys[i];
    return blend(a.pose, b.pose, (t - a.t) / (b.t - a.t));
  }
  const applyAt = t => ed.rig && apply(poseAt(t));
  const duration = () => Math.max(0.2, +$('emoteLength').value || 2);

  function setKey() {
    if (!ed.rig) return;
    const t = Math.round(ed.time * FPS) / FPS;
    const k = ed.keys.find(x => Math.abs(x.t - t) < 0.5 / FPS);
    if (k) k.pose = capture(); else ed.keys.push({t, pose: capture()});
    ed.keys.sort((a, b) => a.t - b.t);
    drawTimeline();
  }

  // ---- picking a bone ----------------------------------------------------------------------------------------
  function select(i) {
    ed.selected = i;
    if (!ed.rig) return;
    ed.rig.dots.forEach((d, j) => d.material.color.set(j === i ? 0xffcf33 : 0x5cc8ff));
    if (i >= 0) {
      tc.attach(ed.rig.bones[i]);
      tc.showX = tc.showY = tc.showZ = true;
      tc.setMode($('emoteMove').checked ? 'translate' : 'rotate');
    } else {
      tc.detach();
    }
    $('boneList').value = i;
    $('boneName').textContent = i >= 0 ? ed.rig.skel.names[i] : 'Click a dot (a joint) on the character';
    $('resetBone').disabled = i < 0;
  }
  function fillBoneList() {
    const l = $('boneList');
    l.innerHTML = '<option value="-1">(no bone)</option>';
    ed.rig.skel.names.forEach((n, i) => l.append(new Option(n, i)));
  }
  $('boneList').onchange = e => select(+e.target.value);
  $('emoteMove').onchange = () => select(ed.selected);
  $('resetBone').onclick = () => {
    const i = ed.selected;
    if (i < 0) return;
    ed.rig.bones[i].quaternion.fromArray(ed.rig.rest[i].q);
    ed.rig.bones[i].position.fromArray(ed.rig.rest[i].p);
    setKey();
  };

  const ray = new THREE.Raycaster();
  function click(e) {
    if (!ed.rig || tc.dragging) return;
    const r = renderer.domElement.getBoundingClientRect();
    ray.setFromCamera(new THREE.Vector2((e.clientX - r.left) / r.width * 2 - 1, -(e.clientY - r.top) / r.height * 2 + 1), camera);
    // the joint dots first (they're drawn on top); else the nearest dot to where the character was hit
    const hit = ray.intersectObjects(ed.rig.dots)[0];
    if (hit) { select(hit.object.userData.bone); return; }
    const body = ray.intersectObjects(holder.children.filter(m => m.isSkinnedMesh))[0];
    if (!body) return;
    let best = -1, bd = Infinity;
    const w = new THREE.Vector3();
    ed.rig.bones.forEach((b, i) => { const d = b.getWorldPosition(w).distanceTo(body.point); if (d < bd) { bd = d; best = i; } });
    select(best);
  }

  // ---- the timeline ------------------------------------------------------------------------------------------
  function drawTimeline() {
    const bar = $('timeKeys'), dur = duration();
    bar.innerHTML = '';
    for (const k of ed.keys) {
      const m = document.createElement('span');
      m.className = 'key';
      m.style.left = (k.t / dur * 100) + '%';
      m.title = k.t.toFixed(2) + ' s';
      m.onclick = () => seek(k.t);
      bar.append(m);
    }
    $('timeSlider').max = dur;
    $('timeLabel').textContent = ed.time.toFixed(2) + ' / ' + dur.toFixed(2) + ' s';
    $('delKey').disabled = !ed.keys.some(k => Math.abs(k.t - ed.time) < 0.5 / FPS);
    $('saveEmote').disabled = !ed.keys.length;
    $('exportGlb').disabled = !ed.rig;
  }
  function seek(t) {
    ed.time = Math.max(0, Math.min(duration(), t));
    $('timeSlider').value = ed.time;
    applyAt(ed.time);
    drawTimeline();
  }
  $('timeSlider').oninput = e => { stop(); seek(+e.target.value); };
  $('emoteLength').onchange = () => { ed.keys = ed.keys.filter(k => k.t <= duration()); seek(ed.time); };
  $('emoteLoop').onchange = () => applyAt(ed.time);
  $('addKey').onclick = setKey;
  $('delKey').onclick = () => { ed.keys = ed.keys.filter(k => Math.abs(k.t - ed.time) >= 0.5 / FPS); seek(ed.time); };
  $('clearKeys').onclick = () => { if (confirm('Remove every key?')) { ed.keys = []; seek(0); } };
  function play() {
    ed.playing = true;
    tc.detach();
    $('playEmote').textContent = 'Stop';
  }
  function stop() {
    if (!ed.playing) return;
    ed.playing = false;
    $('playEmote').textContent = 'Play';
    select(ed.selected);
  }
  $('playEmote').onclick = () => (ed.playing ? stop() : play());
  $('prevKey').onclick = () => { const k = [...ed.keys].reverse().find(x => x.t < ed.time - 1e-4); if (k) seek(k.t); };
  $('nextKey').onclick = () => { const k = ed.keys.find(x => x.t > ed.time + 1e-4); if (k) seek(k.t); };

  function tick(dt) {
    if (!ed.playing || !ed.rig) return;
    let t = ed.time + dt;
    if (t > duration()) {
      if ($('emoteLoop').checked) t %= duration();
      else { t = duration(); stop(); }
    }
    ed.time = t;
    $('timeSlider').value = t;
    applyAt(t);
    $('timeLabel').textContent = t.toFixed(2) + ' / ' + duration().toFixed(2) + ' s';
  }

  // ---- saving / opening --------------------------------------------------------------------------------------
  function motionBin() {
    const dur = duration(), frames = Math.round(dur * FPS) + 1, bones = ed.rig.bones.length + 1;
    const buf = new ArrayBuffer(16 + frames * (4 + bones * 40)), dv = new DataView(buf);
    [0x4f4d4746, 1, bones, frames].forEach((v, i) => dv.setInt32(i * 4, v, true));
    let o = 16;
    const put = v => { dv.setFloat32(o, v, true); o += 4; };
    for (let f = 0; f < frames; f++) {
      const t = Math.min(dur, f / FPS);
      put(t);
      [0, 0, 0, 1, 0, 0, 0, 1, 1, 1].forEach(put);   // (the root: where the game puts the character)
      for (const b of poseAt(t)) { b.q.forEach(put); b.p.forEach(put); [1, 1, 1].forEach(put); }
    }
    return new Uint8Array(buf);
  }
  // the game bones the emote moves (skeleton bone n is the game's n + 1): any a key has away from its rest - the others
  // keep the game's own animation
  function uses() {
    const out = [];
    ed.rig.rest.forEach((r, i) => {
      const moved = ed.keys.some(k => {
        const p = k.pose[i];
        return Math.abs(r.q[0] * p.q[0] + r.q[1] * p.q[1] + r.q[2] * p.q[2] + r.q[3] * p.q[3]) < 0.99999 ||
          Math.hypot(p.p[0] - r.p[0], p.p[1] - r.p[1], p.p[2] - r.p[2]) > 1e-4;
      });
      if (moved) out.push(i + 1);
    });
    return out;
  }
  function buildPack() {
    const name = ($('emoteName').value || $('emoteName').placeholder).trim();
    const o = ed.rig.outfit;
    const manifest = {format: 1, name, character: o.character, outfit: o.outfit, model: o.model.name,
      models: outfitsOf(o.character).map(x => [x.model.lo, x.model.hi]), uses: uses(),
      bones: ed.rig.bones.length + 1, duration: duration(), loop: $('emoteLoop').checked, fps: FPS,
      keys: ed.keys.map(k => ({t: k.t, pose: k.pose})), tool: 'FGUY Skin Studio'};
    // (stored, not compressed: the mod reads them as they are)
    const zip = zipSync({'emote.json': [strToU8(JSON.stringify(manifest)), {level: 0}], 'motion.bin': [motionBin(), {level: 0}]});
    return {name, file: name.replace(/[^\w\- ]+/g, '').trim().replace(/ +/g, '_') + '.fgemote', blob: new Blob([zip])};
  }
  $('saveEmote').onclick = () => {
    if (!ed.rig || !ed.keys.length) return;
    const p = buildPack();
    download(p.blob, p.file);
    setStatus('Saved emote "' + p.name + '" for ' + ed.rig.outfit.character + ' - put it in the game\'s FGUYMod\\emotes folder.');
  };
  $('openEmote').onchange = async e => {
    const f = e.target.files[0];
    e.target.value = '';
    if (!f || !ed.rig) return;
    try {
      const m = JSON.parse(strFromU8(unzipSync(new Uint8Array(await f.arrayBuffer()))['emote.json']));
      if (m.character !== ed.rig.outfit.character) throw new Error('it\'s for ' + m.character + ' - pick that character first');
      ed.keys = m.keys || [];
      $('emoteLength').value = m.duration;
      $('emoteLoop').checked = !!m.loop;
      $('emoteName').value = m.name || '';
      seek(0);
      setStatus('Opened "' + (m.name || f.name) + '".');
    } catch (err) {
      setStatus("Couldn't open that emote: " + err.message);
    }
  };

  // an animation made elsewhere (Blender...) on the character exported from here: bones named bone00, bone01...
  $('importGlb').onchange = async e => {
    const f = e.target.files[0];
    e.target.value = '';
    if (!f || !ed.rig) return;
    try {
      const g = await new GLTFLoader().parseAsync(await f.arrayBuffer(), '');
      const clip = g.animations[0];
      if (!clip) throw new Error('it has no animation');
      const tracks = clip.tracks.filter(t => /^bone\d+\.(quaternion|position)$/.test(t.name));
      if (!tracks.length) throw new Error('its bones aren\'t named bone00, bone01... (export the character from here first)');
      const mixer = new THREE.AnimationMixer(ed.rig.root);
      mixer.clipAction(new THREE.AnimationClip('in', clip.duration, tracks)).play();
      ed.keys = [];
      const step = 1 / 15;
      for (let t = 0; t <= clip.duration + 1e-4; t += step) {
        mixer.setTime(t);
        ed.keys.push({t: Math.round(t * FPS) / FPS, pose: capture()});
      }
      mixer.stopAllAction();
      $('emoteLength').value = Math.max(0.2, clip.duration).toFixed(2);
      seek(0);
      setStatus(`Brought in "${clip.name || f.name}" (${clip.duration.toFixed(2)} s).`);
    } catch (err) {
      setStatus("Couldn't use that file: " + err.message);
    }
  };
  // the rigged character as a .glb - to animate in Blender & co. and bring back with "Import animation"
  $('exportGlb').onclick = async () => {
    if (!ed.rig) return;
    const was = capture();
    apply(ed.rig.rest);
    ed.rig.dots.forEach(d => (d.visible = false));
    tc.detach();
    const glb = await new GLTFExporter().parseAsync(holder, {binary: true});
    ed.rig.dots.forEach(d => (d.visible = true));
    apply(was);
    select(ed.selected);
    download(new Blob([glb]), ed.rig.outfit.model.name + '.glb');
    setStatus('Exported the rigged character - animate it, export a .glb, then "Import animation".');
  };

  return {
    build, tick, click, buildPack,
    leave() { stop(); tc.detach(); },
    get busy() { return tc.dragging; },
  };
}
