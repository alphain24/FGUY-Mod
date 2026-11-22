// Development only (index.html?dev on this PC): helpers to try the motion capture from the browser's console.
//   const t = await import('./dev_mocap_test.js');
//   await t.open('Lois');                              the Emotes tab with a character
//   await t.load('/research/lois_taunt.mp4', 0.3, 4.5); a clip from build/research, its part to use (fetched whole: the
//                                                       development server can't seek in a file)
//   await t.track();                                   -> the Studio's status line
//   await t.strip([0.3, 0.9, ...], 'name.png');        the clip's frames over the character's poses -> build/research
const view = () => window.studioView;
const $ = id => document.getElementById(id);
const wait = ms => new Promise(r => setTimeout(r, ms));

export async function open(character) {
  view().setMode('emotes');
  await wait(800);
  const b = [...document.querySelectorAll('button')].find(x => x.textContent.trim() === character);
  if (!b) return 'no character named ' + character;
  b.click();
  for (let i = 0; i < 40; i++) {
    await wait(250);
    if (view().emotes.rig && view().emotes.rig.outfit.character === character) return 'ok: ' + view().emotes.rig.skel.count + ' bones';
  }
  return 'not ready';
}

export async function load(url, start, length) {
  const blob = await (await fetch(url)).blob();
  view().mocap.setVideo(URL.createObjectURL(blob), url.split('/').pop());
  for (let i = 0; i < 40 && !($('mocapVideo').readyState >= 1); i++) await wait(250);
  await wait(300);
  $('mocapStart').value = start;
  $('mocapLength').value = length;
  return 'ok: ' + $('mocapVideo').duration.toFixed(2) + ' s';
}

// (started here, finished later: a tracking run is longer than one console call may take - done() says when)
let running = null;
export function track() { running = view().mocap.track().then(() => (running = null)); return 'started'; }
export const done = () => (running ? 'tracking ' + $('mocapBar').style.width : $('status2').textContent);

// tracked points kept for another run (tracking takes a minute): saved to build/research, put back on the character
export async function saveFrames(name) {
  const r = await fetch('/save/' + name, {method: 'PUT', body: JSON.stringify(view().mocap.frames)});
  return r.status + ' ' + view().mocap.frames.length;
}
export async function useFrames(name) {
  const frames = await (await fetch('/research/' + name)).json();
  view().mocap.usePoints(frames, frames.map((_, i) => i / 30));
  return 'ok: ' + frames.length + ' frames, ' + view().emotes.keys.length + ' keys';
}
// the whole character in the 3D view's picture
export async function frameAll() {
  const THREE = await import('three'), v = view(), c = v.camera;
  const box = new THREE.Box3().setFromObject(v.holder), h = box.max.y - box.min.y, cy = (box.max.y + box.min.y) / 2;
  c.position.set(0, cy, (h * 0.62) / Math.tan(c.fov * Math.PI / 360));
  v.controls.target.set(0, cy, 0);
  c.lookAt(0, cy, 0);
  c.updateProjectionMatrix();
}

export function seek(t) {
  if ($('playEmote').textContent === 'Stop') $('playEmote').click();
  $('timeSlider').value = t;
  $('timeSlider').dispatchEvent(new Event('input'));
}

// the clip's frames over the character from the front, three-quarters and the side -> build/research
// (zoom, at: nearer by that much, looking at that part of the character's height - 2, 0.8: the head and shoulders)
export async function views(times, name, size = 300, angles = [0, 40, 90], zoom = 1, at = 0.5) {
  const THREE = await import('three'), vw = view(), c = vw.camera;
  const v = $('mocapVideo'), start = +$('mocapStart').value, W = size, H = size;
  const out = document.createElement('canvas');
  out.width = W * times.length;
  out.height = H * (1 + angles.length);
  const g = out.getContext('2d');
  g.fillStyle = '#000';
  g.fillRect(0, 0, out.width, out.height);
  const box = new THREE.Box3().setFromObject(vw.holder), h = box.max.y - box.min.y, cy = box.min.y + h * at;
  const far = (h * 0.62) / Math.tan(c.fov * Math.PI / 360) / zoom;
  for (let i = 0; i < times.length; i++) {
    const t = times[i];
    await new Promise(ok => { v.addEventListener('seeked', () => ok(), {once: true}); v.currentTime = start + t + 0.0001; });
    const s = Math.min(W / v.videoWidth, H / v.videoHeight);
    g.drawImage(v, i * W + (W - v.videoWidth * s) / 2, (H - v.videoHeight * s) / 2, v.videoWidth * s, v.videoHeight * s);
    seek(t);
    for (let k = 0; k < angles.length; k++) {
      const a = angles[k] * Math.PI / 180;
      c.position.set(Math.sin(a) * far, cy, Math.cos(a) * far);
      vw.controls.target.set(0, cy, 0);
      c.lookAt(0, cy, 0);
      c.updateProjectionMatrix();
      g.drawImage(vw.shot(W * 2, H * 2), i * W, H * (k + 1), W, H);
    }
  }
  const blob = await new Promise(r => out.toBlob(r, 'image/png'));
  const res = await fetch('/save/' + name, {method: 'PUT', body: blob});
  return res.status + ' ' + blob.size;
}

export async function strip(times, name, size = 300) {
  const v = $('mocapVideo'), start = +$('mocapStart').value, W = size, H = size;
  const out = document.createElement('canvas');
  out.width = W * times.length;
  out.height = H * 2;
  const g = out.getContext('2d');
  g.fillStyle = '#000';
  g.fillRect(0, 0, out.width, out.height);
  for (let i = 0; i < times.length; i++) {
    const t = times[i];
    await new Promise(ok => { v.addEventListener('seeked', () => ok(), {once: true}); v.currentTime = start + t + 0.0001; });
    const s = Math.min(W / v.videoWidth, H / v.videoHeight);
    g.drawImage(v, i * W + (W - v.videoWidth * s) / 2, (H - v.videoHeight * s) / 2, v.videoWidth * s, v.videoHeight * s);
    seek(t);
    g.drawImage(view().shot(W * 2, H * 2), i * W, H, W, H);
  }
  const blob = await new Promise(r => out.toBlob(r, 'image/png'));
  const res = await fetch('/save/' + name, {method: 'PUT', body: blob});
  return res.status + ' ' + blob.size;
}
