# Reference suitability (grimoire/intake/validation_rubric.md)

Verdict: **conditional -> stylized** (character-conditional route not needed: the target is the outfit pieces, objects
worn on an existing rigged character).

Pass criteria met:
- one obvious target (the outfit on one figure), full figure in frame, strong silhouette for hat, vest, boots, sheath;
- major materials visible (felt, leather, brass, laces);
- target approximates well with procedural primitives (lathe/torus brim, offset body shells, extruded pockets, tubes).

Conditional because:
- single view, 270 x 507 (probe_image.py: technicalSuitability conditional, low resolution) - rivets/eyelets approximate;
- hidden back of hat / vest / boots inferred by bilateral symmetry and a plain back panel;
- flat cartoon paint: stylized solid albedo per part is the intended finish (accepted: the game is cel-shaded).

Not required: exact geometry, logos, text.
