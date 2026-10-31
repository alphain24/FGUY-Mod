// Close-up renders of what the Skin Studio shows, for checking a model by eye, saved to build/research (PUT /save/):
//   (await import('/content/peter-adventurer/views.js')).shoot('name', [x, y, z] camera, [x, y, z] target, size?)
// Its own renderer on the Studio's scene (the holder's parent), so the page's layout doesn't matter.
import * as THREE from 'three';
let renderer;
export async function shoot(name, pos, target, size = 900, fov = 30) {
  const scene = window.studioView.holder.parent;
  renderer ??= new THREE.WebGLRenderer({antialias: true, preserveDrawingBuffer: true});
  renderer.setSize(size, size, false);
  renderer.outputColorSpace = THREE.SRGBColorSpace;
  const cam = new THREE.PerspectiveCamera(fov, 1, 0.01, 50);
  cam.position.set(...pos);
  cam.lookAt(...target);
  renderer.render(scene, cam);
  const blob = await new Promise(r => renderer.domElement.toBlob(r, 'image/png'));
  await fetch('/save/view_' + name + '.png', {method: 'PUT', body: blob});
  return name;
}
