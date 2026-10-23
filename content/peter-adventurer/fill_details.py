"""Fills the detail inventory (zone scan of reference.png) into di.json and assessment.json.

Each detail maps to a component (or its local feature) of object-sculpt-spec.json."""
import json

# id, kind, description, region (x, y, w, h normalized), scale, affects, mapsTo ref, zone, confidence
D = [
    ('hat-brim-curl', 'ridge', 'brim rolled up at both lateral sides, dipping front and back', (0.15, 0.08, 0.6, 0.12),
     'meso', 'silhouette', 'hat-brim', 'r0c1', 0.85),
    ('hat-band', 'ridge', 'slate blue band wrapping the crown base, slightly proud of the felt', (0.32, 0.1, 0.33, 0.04),
     'meso', 'albedo+relief', 'hat-band', 'r0c1', 0.9),
    ('hat-crown-crease', 'groove', 'front-to-back pinch crease along the crown top', (0.35, 0.0, 0.25, 0.08),
     'micro', 'relief', 'hat-crown', 'r0c1', 0.7),
    ('vest-chest-pocket-flap', 'seam', 'flap pocket on each chest panel, flap edge a raised seam, one brass button',
     (0.35, 0.42, 0.55, 0.1), 'meso', 'relief', 'vest-chest-pockets', 'r1c1', 0.8),
    ('vest-lower-pocket-flap', 'seam', 'large flap pocket on each lower front panel, one brass button',
     (0.3, 0.52, 0.65, 0.12), 'meso', 'relief', 'vest-lower-pockets', 'r1c1', 0.85),
    ('vest-panel-studs', 'fastener', 'brass studs down each front panel edge (~3 per side)', (0.48, 0.5, 0.35, 0.16),
     'micro', 'albedo+relief', 'vest-studs', 'r1c1', 0.6),
    ('vest-open-front', 'seam', 'open front: shirt placket visible between the two front panels', (0.5, 0.4, 0.28, 0.27),
     'macro', 'silhouette', 'vest-shell', 'r1c1', 0.85),
    ('pendant-claw', 'ridge', 'black curled claw hook at the sternum below the collar', (0.53, 0.36, 0.1, 0.08),
     'meso', 'silhouette', 'pendant-claw', 'r1c1', 0.8),
    ('sheath-right-hip', 'seam', 'ochre tan curved blade sheath hanging off the belt on the wearer\'s right hip',
     (0.12, 0.68, 0.18, 0.17), 'macro', 'silhouette', 'sheath-body', 'r2c0', 0.85),
    ('sheath-strap-stud', 'fastener', 'strap over the belt at the sheath mouth with one stud', (0.18, 0.68, 0.12, 0.05),
     'micro', 'relief', 'sheath-strap', 'r2c0', 0.6),
    ('boot-laces', 'stitch', 'black criss-cross laces up the front of each boot', (0.35, 0.78, 0.45, 0.12),
     'micro', 'albedo+relief', 'boot-laces', 'r2c1', 0.85),
    ('boot-eyelets', 'fastener', 'brass eyelets in two columns along each lace panel', (0.35, 0.78, 0.45, 0.12),
     'micro', 'albedo', 'boot-eyelets', 'r2c1', 0.6),
    ('boot-toe-sole', 'ridge', 'bulbous toe box over a thick sole', (0.2, 0.88, 0.7, 0.12),
     'meso', 'silhouette', 'boot-shells', 'r2c1', 0.8),
    ('cuff-roll-button', 'fastener', 'rolled sleeve band with a strap tab and brass button on the outer side',
     (0.04, 0.44, 0.93, 0.1), 'micro', 'relief', 'sleeve-cuffs', 'r1c0', 0.7),
]
details = []
for i, k, t, (x, y, w, h), sc, af, ref, zone, conf in D:
    details.append({
        'id': i, 'kind': k, 'description': t,
        'region': {'x': x, 'y': y, 'width': w, 'height': h, 'units': 'normalized'},
        'scale': sc, 'affects': af, 'mapsTo': {'type': 'component', 'ref': ref},
        'evidenceRef': f'detail-inventory/zone-{zone}.png', 'confidence': conf,
    })
di = json.load(open('di.json', encoding='utf-8'))
di['detailInventory']['scanMethod'] = 'grid-3x3'
di['detailInventory']['targetMinDetails'] = 10
di['detailInventory']['details'] = details
json.dump(di, open('di.json', 'w', encoding='utf-8'), indent=2, ensure_ascii=False)
a = json.load(open('assessment.json', encoding='utf-8'))
inv = a['preSpecAssessment']['detailInventory']
inv['scanMethod'] = 'grid-3x3'
inv['details'] = details
json.dump(a, open('assessment.json', 'w', encoding='utf-8'), indent=2, ensure_ascii=False)
print('ok', len(details))
