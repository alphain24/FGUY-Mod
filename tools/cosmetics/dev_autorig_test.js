// Development only (index.html?dev): a made-up person to try the auto-rigger with - capsules for the limbs, a box-ish torso, a
// head - standing in a T- or an A-pose, as a .glb the Models tab can bring in. Not a model anyone wears: a test shape.
//   const t = await import('./dev_autorig_test.js?' + Date.now());
//   const g = await t.human({pose: 'A', height: 1.8});   -> {glb: Blob, truth: {landmark: [x, y, z]}}
//   await t.bringIn(g.glb, 'Test person');                the Models tab imports it
import * as THREE from 'three';
import {GLTFExporter} from 'three/addons/exporters/GLTFExporter.js';
import {mergeGeometries} from 'three/addons/utils/BufferGeometryUtils.js';

const V = (x, y, z) => new THREE.Vector3(x, y, z);

// a capsule from a to b, radius r
function limb(a, b, r) {
  const len = a.distanceTo(b), g = new THREE.CapsuleGeometry(r, Math.max(0.001, len), 6, 12);
  const dir = b.clone().sub(a).normalize(), q = new THREE.Quaternion().setFromUnitVectors(V(0, 1, 0), dir);
  g.applyQuaternion(q);
  g.translate((a.x + b.x) / 2, (a.y + b.y) / 2, (a.z + b.z) / 2);
  return g;
}

export async function human({pose = 'T', height = 1.8, headScale = 1} = {}) {
  const H = height, T = {};
  T.ankleL = V(0.1 * H, 0.045 * H, 0); T.kneeL = V(0.1 * H, 0.28 * H, 0.01 * H); T.hipL = V(0.09 * H, 0.52 * H, 0);
  T.toeL = V(0.1 * H, 0.01 * H, 0.14 * H);
  T.pelvis = V(0, 0.53 * H, 0); T.chest = V(0, 0.79 * H, 0); T.neck = V(0, 0.84 * H, 0);
  T.head = V(0, 0.87 * H, 0); T.top = V(0, H, 0);
  T.shoulderL = V(0.19 * H, 0.815 * H, 0);
  const armDir = pose === 'A' ? V(Math.cos(Math.PI / 4), -Math.sin(Math.PI / 4), 0) : V(1, 0, 0);
  T.elbowL = T.shoulderL.clone().addScaledVector(armDir, 0.17 * H);
  T.wristL = T.elbowL.clone().addScaledVector(armDir, 0.15 * H);
  T.tipL = T.wristL.clone().addScaledVector(armDir, 0.1 * H);
  for (const k of Object.keys(T)) if (/L$/.test(k)) { const p = T[k]; T[k.replace(/L$/, 'R')] = V(-p.x, p.y, p.z); }
  const parts = [];
  for (const s of ['L', 'R']) {
    parts.push(limb(T['hip' + s], T['knee' + s], 0.06 * H), limb(T['knee' + s], T['ankle' + s], 0.045 * H));
    parts.push(limb(T['ankle' + s], T['toe' + s], 0.035 * H));
    parts.push(limb(T['shoulder' + s], T['elbow' + s], 0.04 * H), limb(T['elbow' + s], T['wrist' + s], 0.032 * H));
    parts.push(limb(T['wrist' + s], T['tip' + s], 0.025 * H));
  }
  const torso = new THREE.CapsuleGeometry(0.13 * H, 0.12 * H, 6, 16);   // (its bottom at 0.49: the crotch; its top 0.87)
  torso.scale(1, 1, 0.6);
  torso.translate(0, 0.68 * H, 0);
  parts.push(torso, limb(T.neck.clone().setY(0.8 * H), T.head, 0.04 * H));
  const head = new THREE.SphereGeometry(0.065 * H * headScale, 20, 14);
  head.translate(0, T.head.y + 0.065 * H * headScale, 0.01 * H);
  T.top.y = T.head.y + 0.13 * H * headScale;
  parts.push(head);
  const geo = mergeGeometries(parts.map(g => g.toNonIndexed()));
  const mesh = new THREE.Mesh(geo, new THREE.MeshStandardMaterial({color: 0xc8a27a}));
  const scene = new THREE.Scene();
  scene.add(mesh);
  const glb = await new GLTFExporter().parseAsync(scene, {binary: true});
  const truth = Object.fromEntries(Object.entries(T).map(([k, p]) => [k, [p.x, p.y, p.z].map(x => +x.toFixed(3))]));
  return {glb: new Blob([glb]), truth, mesh};
}

export async function bringIn(blob, name = 'Test person') {
  const input = document.getElementById('importModel');
  const file = new File([blob], name + '.glb');
  const dt = new DataTransfer();
  dt.items.add(file);
  input.files = dt.files;
  input.dispatchEvent(new Event('change'));
  await new Promise(r => setTimeout(r, 1500));
  return document.getElementById('status2')?.textContent;
}
