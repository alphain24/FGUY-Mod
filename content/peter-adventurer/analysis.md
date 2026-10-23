# Peter adventurer outfit - image analysis (img2threejs layered protocol)

Reference: `reference.png` (270 x 507, transparent background, flat cel-shaded 2D cartoon art, 3/4 front view, character
facing camera-left slightly). Low resolution: details under ~3 px (rivet heads, lace eyelets) are approximate.

Target: NOT the whole figure. The wearer is Peter Griffin, already a complete rigged game model (Peter_Rig_Geo, 58 bones,
drawn by the game); this reconstruction builds the **outfit pieces that are not on Peter's own model**, as separate
procedural meshes fitted on his T-pose bind mesh (`build/research/peter_rig.glb`, used only as a fitting reference, never
copied). The recolorable parts (shirt, trousers, belt) are done as a skin recipe on Peter's own texture instead.

## Layer 1 - identification
- Work type: costume accessory set (headwear, upper-body garment layer, footwear, belt-hung sheath, neck pendant).
- primaryDomain: `object` (garments/props worn on an existing character); confidence 0.9.

## Layer 2 - form & silhouette
- Hat: wide-brim outback/cowboy hat - torus-like brim, lateral sides rolled up (curl), front/back dipping; crown an
  elongated dome with a central front-to-back crease (pinch). Bilateral symmetry.
- Vest: open-front sleeveless shell following the torso (continuous surface over a large rounded belly), armholes,
  hem at belt height; bilateral with asymmetric pocket detail allowed (mirrored pockets).
- Boots: two high lace-up boots, rounded bulbous toe, ankle shaft to mid-shin; mirrored pair.
- Sheath: flat curved blade-shaped leather sheath hanging from the belt, tapering to a point; on the wearer's RIGHT hip
  (image left - 3D: wearer's right = -X).
- Pendant: small curled hook/claw (fang) shape at the sternum, hanging from a cord (cord hidden by the collar).
- Sleeve cuffs: rolled cuff bands around each short sleeve end, with a strap + button on the outer side.

## Layer 3 - macro -> meso -> micro
- hat (macro): crown, brim; meso: hat band (around crown base); micro: crown crease.
- vest (macro): left panel, right panel, back panel; meso: chest pocket + flap (x2), lower pocket + flap (x2), armhole edge;
  micro: brass rivets/buttons (flap buttons, panel studs), seam lines.
- boots (macro x2): shaft, toe box, sole; meso: lace panel (eyelets column x2); micro: crossed laces, eyelets.
- sheath (macro): sheath body; meso: belt loop strap; micro: stud.
- pendant (macro): claw hook; micro: none.
- cuffs (macro x2): roll band; meso: strap tab; micro: brass button.

## Layer 4 - spatial relationships
- <hat, sits-on, head top> overlap (crown wraps skull top, brim above the ears / glasses line).
- <vest, overlaps, shirt torso> shell offset outward from the torso surface; open in front (shirt placket visible).
- <boots, wrap, lower legs + feet> shell over Peter's shoes and lower trousers.
- <sheath, attached-to, belt at right hip> hangs below the belt, outside the trouser leg.
- <pendant, attached-to, neck/sternum> just under the collar opening.
- <cuff, wraps, sleeve end> ring at the short-sleeve hem.

## Layer 5/6 - materials & color (flat cel paint - solid albedo, matte)
| part | albedo (sRGB, observed flat value) | finish |
|---|---|---|
| hat felt | rgb(96, 64, 62) dark mauve-brown | matte, roughness ~0.9 |
| hat band | rgb(70, 95, 130) slate blue | matte |
| vest leather | rgb(110, 62, 52) dark red-brown; pocket flaps a touch lighter rgb(130, 76, 64) | satin-matte, roughness ~0.7 |
| brass rivets/buttons/buckle | rgb(196, 148, 62) | metallic paint look (cel: flat) |
| boots leather | rgb(96, 58, 52) dark brown | satin-matte |
| laces | rgb(30, 24, 24) near-black | matte |
| sheath | rgb(186, 136, 52) ochre tan | matte |
| pendant | rgb(28, 28, 30) black | gloss-ish |
| shirt (skin recolor) | rgb(140, 186, 120) sage green; buttons light grey | - |
| trousers (skin recolor) | rgb(96, 116, 146) slate blue (jeans) | - |
| belt (skin recolor) | rgb(90, 56, 48) brown | - |
The art style is flat cartoon paint with ink outlines: solid albedo per part is the right finish (no texture
projection: the Studio's palette texture carries the colors).

## Layer 7 - identity-defining features
1. Wide hat brim curled up at the sides + blue band (most recognizable).
2. Dark brown leather vest with big flap pockets and brass buttons.
3. Black claw pendant at the sternum.
4. Tan curved knife sheath on the right hip.
5. Brown lace-up boots with black crossed laces.
6. Green shirt with rolled cuffs (skin + cuff rolls), blue jeans (skin).

## Layer 8 - uncertainty
- Back of the hat, vest back and boot heels are hidden: vest back assumed a plain panel, hat symmetric.
- Pocket count on the hidden side mirrored from the visible side.
- Low resolution: rivet count approximate (chest flap 1 button, lower flap 1 button, panel studs ~3 per side).
