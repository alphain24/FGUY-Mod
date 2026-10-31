// The Skin Studio steps that make Peter_Adventurer.fgskin, run in the Studio's page (index.html?dev, served by
// tools/cosmetics/dev_server.py): (await import('/content/peter-adventurer/studio_build.js')).build()
//   1. Models tab, Peter (Default): import the outfit GLB (review/index.html's exportOutfit -> build/research), saved
//      as a model pack worn over him (its weights copied from him, the triangles of his under it listed to hide).
//   2. Skins tab: his texture recolored (shirt, jeans, buckle), the belt and the glasses' lenses painted (our own
//      pixels: only where his texture is near black on the belt band / near white on the face), the model added, the
//      reference drawing as its picture in the game's menus (its skin tile, the HUD's head).
//   3. Both written to build/research (PUT /save/<file>); nothing is downloaded.
const sleep = ms => new Promise(r => setTimeout(r, ms));
const $ = id => document.getElementById(id);
const hex = c => '#' + c.map(v => v.toString(16).padStart(2, '0')).join('');

const RECOLORS = [   // [from, to, "How close" slider]
  [[228, 228, 225], [127, 160, 109], 25],   // white shirt -> sage green
  [[37, 99, 8], [79, 94, 114], 18],         // green trousers -> slate blue jeans
  [[247, 174, 41], [150, 114, 46], 14],     // buckle (inside) -> brass
  [[255, 120, 47], [128, 92, 30], 14],      // buckle (rim) -> darker brass
];

async function waitFor(test, ms = 20000) {
  for (let t = 0; t < ms; t += 250) { if (test()) return true; await sleep(250); }
  return false;
}

// our own pixels on the triangles of his whose vertices are all in a band: where his texture is near `from`
function paint(t, {yLo, yHi, zMin = -9, src, from, rad, to, pick = 30}) {
  const W = t.canvas.width, H = t.canvas.height, img = t.ctx.getImageData(0, 0, W, H), d = img.data;
  const dist = (a, i, c) => Math.hypot(a[i] - c[0], a[i + 1] - c[1], a[i + 2] - c[2]);
  const area = (a, b, c) => (b[0] - a[0]) * (c[1] - a[1]) - (c[0] - a[0]) * (b[1] - a[1]);
  const S = src === 'original' ? t.original.data : d;
  let tris = 0, px = 0;
  for (const mesh of window.studioView.holder.children) {
    if (mesh.userData.texture !== t) continue;
    const pos = mesh.geometry.attributes.position, uv = mesh.geometry.attributes.uv, idx = mesh.userData.piece.index;
    for (let k = 0; k < idx.length; k += 3) {
      const v = [idx[k], idx[k + 1], idx[k + 2]];
      if (!v.every(i => pos.getY(i) > yLo && pos.getY(i) < yHi && pos.getZ(i) > zMin)) continue;
      const U = v.map(i => [((uv.getX(i) % 1) + 1) % 1 * W, ((uv.getY(i) % 1) + 1) % 1 * H]);
      const c = (Math.floor((U[0][1] + U[1][1] + U[2][1]) / 3) * W + Math.floor((U[0][0] + U[1][0] + U[2][0]) / 3)) * 4;
      if (dist(S, c, from) > pick) continue;
      const A = area(U[0], U[1], U[2]);
      if (Math.abs(A) < 1e-9) continue;
      tris++;
      const xs = U.map(u => u[0]), ys = U.map(u => u[1]);
      for (let y = Math.floor(Math.min(...ys)); y <= Math.ceil(Math.max(...ys)); y++)
        for (let x = Math.floor(Math.min(...xs)); x <= Math.ceil(Math.max(...xs)); x++) {
          const P = [x + 0.5, y + 0.5], w0 = area(U[1], U[2], P) / A, w1 = area(U[2], U[0], P) / A;
          if (w0 < -0.02 || w1 < -0.02 || 1 - w0 - w1 < -0.02 || x < 0 || y < 0 || x >= W || y >= H) continue;
          const i = (y * W + x) * 4, e = dist(S, i, from);
          if (e > rad) continue;
          const f = 1 - e / rad;
          for (let j = 0; j < 3; j++) d[i + j] = d[i + j] * (1 - f) + to[j] * f;
          px++;
        }
    }
  }
  t.ctx.putImageData(img, 0, 0);
  t.tex.needsUpdate = true;
  t.modified = true;
  return {tris, px};
}

export async function build() {
  const studio = window.studio, out = {};
  const peter = () => studio.outfits.find(o => o.model.name === 'Peter_Rig_Geo');
  await waitFor(() => studio.outfits?.length && peter());
  // 1. the model
  $('tabModels').click();
  peter().chip.click();
  await waitFor(() => window.studioView.models.state.outfit === peter() && window.studioView.models.state.solid);
  // (the outfit with its vest made from Peter's own surface - vest_from_body.js - over the first GLB's)
  const glb = await (await fetch('/research/peter_adventurer_outfit_v2.glb', {cache: 'no-store'})).blob();
  let dt = new DataTransfer();
  dt.items.add(new File([glb], 'Peter Adventurer.glb'));
  $('importModel').files = dt.files;
  $('importModel').dispatchEvent(new Event('change'));
  await waitFor(() => window.studioView.models.state.src);
  for (const id of ['fitKeep', 'modelOver']) {   // (made on him, worn over him - whatever the Studio guessed)
    if (!$(id).checked) { $(id).checked = true; $(id).dispatchEvent(new Event('change')); }
  }
  window.studioView.models.state.hidden = null;
  await waitFor(() => window.studioView.models.state.hidden?.length);
  $('modelName').value = 'Peter Adventurer';
  const click = HTMLAnchorElement.prototype.click;   // (no download: the bytes go to build/research instead)
  HTMLAnchorElement.prototype.click = function () { if (!this.href.startsWith('blob:')) click.call(this); };
  try {
    window.studioView.models.state.saved = null;
    $('saveModel').click();
    await waitFor(() => window.studioView.models.state.saved);
    const model = window.studioView.models.state.saved;
    await fetch('/save/' + model.file, {method: 'PUT', body: model.blob});
    out.model = $('status3').textContent;
    out.hidden = window.studioView.models.state.hidden.map(h => [h.indices, h.hidden]);
    // 2. the skin
    $('tabSkins').click();
    await waitFor(() => studio.textures.get('Peter_Char_CO') && studio.current === peter() && studio.current.textures);
    const t = studio.textures.get('Peter_Char_CO');
    $('reset').disabled = false;
    for (const [P, Q, slider] of RECOLORS) {
      studio.picked = {t, rgb: P};
      $('apply').disabled = false;
      $('toColor').value = hex(Q);
      $('tolerance').value = slider;
      $('apply').click();
    }
    out.belt = paint(t, {yLo: 0.54, yHi: 0.70, src: 'now', from: [16, 16, 16], rad: 45, to: [62, 44, 40]});
    out.lenses = paint(t, {yLo: 1.66, yHi: 1.9, zMin: 0.12, src: 'original', from: [228, 228, 225], rad: 40, to: [232, 232, 230]});
    $('packName').value = 'Peter Adventurer';
    dt = new DataTransfer();
    dt.items.add(new File([model.blob], 'Peter_Adventurer.fgmodel'));
    $('packModel').files = dt.files;
    $('packModel').dispatchEvent(new Event('change'));
    await waitFor(() => /With its model/.test($('packModelInfo').textContent));
    dt = new DataTransfer();   // (its picture in the menus: the reference drawing itself)
    dt.items.add(new File([await (await fetch('/content/peter-adventurer/reference.png')).blob()], 'reference.png', {type: 'image/png'}));
    $('packPicture').files = dt.files;
    $('packPicture').dispatchEvent(new Event('change'));
    await waitFor(() => studio.packPicture);
    const pack = await studio.buildPack();
    await fetch('/save/Peter_Adventurer.fgskin', {method: 'PUT', body: pack.blob});
    out.skin = {bytes: pack.blob.size, textures: pack.count, ops: t.ops.length};
  } finally {
    HTMLAnchorElement.prototype.click = click;
  }
  return out;
}
