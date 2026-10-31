# Peter: Adventurer (FGUY Cosmetics)

Peter's adventurer outfit, worn **over** his own model: a wide-brim felt hat with a blue band, an open leather vest
with flap pockets and brass buttons, a black claw pendant, a knife sheath on his right hip, lace-up boots and rolled
sleeve cuffs. His shirt, jeans, belt and buckle are a recolour of his own texture. In the mod it is one skin pack,
`dist/FGUYMod/mods/FGUYCosmetics/skins/Peter_Adventurer.fgskin`: the recolour recipe (no game art) and the outfit's
model pack inside it (`model.fgmodel`, our own mesh and a 10-colour palette). Switch it on for Peter (Default) on the
MODS page or pick it in the lobby; online it goes to the other players with the skin. In the Skin Studio it is the
"★ Adventurer" chip beside Peter's outfits (tools/cosmetics/library.json lists the mod's skins).

## How it was made (img2threejs)

Reconstructed from a reference picture with the img2threejs skill, then skinned in the Skin Studio:

1. `analysis.md`, `suitability.md`, `assessment.json`, `di.json`: what the picture shows, part by part (14 details).
2. `measure_peter.py`: Peter's bind mesh (the Studio's `peter_rig.glb` export of your game) measured - torso, leg,
   arm and head sections, bone positions - into `measure.json`, so every piece is sized to him.
3. `build_spec.py` (run by `rebuild_spec.sh`): the sculpt spec, `object-sculpt-spec.json` - 70 components in
   Peter's bind space. The vest, hat, boots and sheath are implicit (SDF) shells with rounded trims over their cut
   edges; pockets, pendant, laces, eyelets and buttons sit on the surface the factory builds (`sdf_eval.py`, the
   factory's SDF evaluated in Python). Colours: each material's lit tone measured from a crop of the picture.
4. `regen.sh <pass>`: the Three.js factory (`src/createObjectModel.ts`, and a `.js` for the review page).
5. `review/index.html` (served by `tools/cosmetics/dev_server.py` at `/content/peter-adventurer/review/`): the
   outfit on Peter, arms lowered as in the picture, with a preview of the recolour; `captureAll`, `partsManifest`,
   `exportOutfit` (the GLB the Studio imports). `gate.sh <pass> <prefix>`: the skill's review gates.
6. Skin Studio - all of it in `studio_build.js` (run in the Studio's page: `(await import('/content/peter-adventurer/
   studio_build.js')).build()`): Models tab - import the GLB (kept in place, worn over him; each piece's weights from
   his vertices beneath it, laces / eyelets / buttons riding the piece they sit on, the hat and sheath rigid), the
   triangles of his deep inside the boots and the crown listed to hide; Skins tab - four recolours (shirt, jeans,
   buckle), the belt and the glasses' lenses painted as our own pixels, the model added; both saved.

His shirt showed through the vest at the back and shoulders (the vest sat up to 0.1 inside him there) and his hip
through the sheath: the Studio now moves a worn-over model out of the character wherever he's drawn
(`docs/models.md`, "Nothing of the character showing through"). The vest's sides also followed his upper arms (447 of
its 1,157 vertices took arm weights) and swung onto the gun, and 169 of the outfit's triangles were wound backwards
(black specks on the vest's back): both fixed in the Studio for every worn-over model (`docs/models.md`). `clash_check.js` (run in the Studio's page like
`studio_build.js`) measures it - per part of the outfit, how many vertices are inside him and how deep.

**The vest, remade from Peter himself** (`vest_from_body.js`, run in the Studio's page; it writes
`build/research/peter_adventurer_outfit_v2.glb`, which `studio_build.js` puts on him). The first vest - an implicit
shell meshed at the skill's resolution - had stair-stepped straps and edges, and an oval hole on each side under the
arm. Now it is his own body-and-shirt surface where the first vest covered him: that coverage smoothed in 3D (so the
outline - fronts, hem, armholes, collar line, straps - keeps the first vest's shape as smooth curves), the side holes
filled (only what reaches an opening stays open: arms, neck, legs, the front opening), the armholes cut by his arm
bones (not by how far out a point is - his belly is as wide as his shoulders), cut along that outline through his
triangles, the edges smoothed, and lifted 0.02 off him with 0.016 of leather (outer, inner and a rim round every
edge). Every point lies over one of his, so it bends exactly as he does. The pockets, flaps and buttons are seated on
it again. `pose_check.js` poses him and the outfit (arms down / forward, leaning, a leg up, head turned) and lists
what of the outfit is then inside him: nothing of the vest at rest or leaning; with the arms swung through his belly
(the test poses do that - his own arm goes through his own body there), the arm, not the vest.

The boots end below his knee (at 0.29; the knee is at 0.32): over the knee his trousers bulged out of them whenever it
bent. They cover his legs without touching each other (inner edges at x 0.0003).

Passes reviewed: blockout, structural, form refinement (each with a refine-spec round). The staged pipeline was
stopped at the material pass (recorded in the spec's review history): the colours are checked directly against the
picture (all ten within RGB 3.6), but the skill's Tier 1 per-part colour check can't pass for this subject - it
compares each part to five colour clusters of the whole frame, and the 33 small brass fasteners never make one (the
reference picture itself fails it: `review/tier1_reference_as_render.json`). The remaining passes (surface,
lighting, interaction) are for a Three.js presentation; in the game its own toon shader lights the pieces. The
triangle budget is in the spec (per-part decimation): 16,720 triangles, Peter himself is 8,680.

The reference picture, crops of it and renders showing the game's Peter stay on this PC (`.gitignore`).
