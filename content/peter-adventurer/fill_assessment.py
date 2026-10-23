"""Fills assessment.json's preSpecAssessment from analysis.md (img2threejs pre-spec gate)."""
import json

a = json.load(open('assessment.json', encoding='utf-8'))
p = a['preSpecAssessment']
p['objectClass'] = {
    'primaryType': 'costume accessory set (hat, vest, boots, sheath, pendant, sleeve cuffs) worn on a rigged cartoon character',
    'primaryDomain': 'object',
    'formLanguage': ['organic shells offset from a body surface', 'lathed brim and crown', 'extruded flat pockets', 'tapered sheath'],
    'structureKind': ['wearable layer', 'mirrored pairs', 'belt-hung attachment'],
    'motionPotential': ['skinned to the wearer\'s skeleton (deforms with the body)'],
    'materialFamilies': ['felt', 'leather', 'brass', 'cord'],
    'notes': 'Worn by Peter Griffin (the game\'s own rigged model, not rebuilt). Pieces are fitted on his T-pose bind mesh '
             '(peter_rig.glb, a fitting reference only) and skinned to his skeleton in the Skin Studio.',
}
p['complexity']['scores'] = {
    'silhouetteComplexity': 3, 'componentCount': 4, 'hierarchyDepth': 3, 'repetitionDensity': 3,
    'materialLayerCount': 3, 'localDetailDensity': 3, 'occlusionRisk': 2, 'actionReadinessNeed': 3,
}
p['complexity']['estimatedCounts'] = {
    'macroComponents': 6, 'mesoComponents': 12, 'microFeatureGroups': 6, 'materialLayers': 8, 'repetitionSystems': 4,
}
p['complexity']['reasoning'] = [
    'Six macro pieces (hat, vest, boot pair, sheath, pendant, cuff pair), each with meso sub-assemblies (band, pockets + '
    'flaps, lace panels, strap) and micro repetition (rivets, eyelets, crossed laces).',
    'Organic shells must follow the wearer\'s body surface: offset from his bind mesh rather than primitive boxes.',
]
p['unknownsToResolveBeforeImplementation'] = [
    'Back of the hat, vest back and boot heels are hidden: symmetric hat, plain vest back panel, mirrored boots assumed.',
    'Rivet / eyelet counts approximate at this resolution (270 x 507).',
    'Pendant cord hidden by the collar: no cord modelled.',
]
details = [
    ('hat-brim-curl', 'hat', 'brim rolled up at the lateral sides, dipping front and back', 'hat.brim'),
    ('hat-band', 'hat', 'slate blue band around the crown base', 'hat.band'),
    ('hat-crown-crease', 'hat', 'front-to-back crease on top of the crown', 'hat.crown'),
    ('vest-chest-pocket-flap', 'vest', 'flap pocket on each chest panel with one brass button', 'vest.chestPocket'),
    ('vest-lower-pocket-flap', 'vest', 'large flap pocket on each lower front panel with one brass button', 'vest.lowerPocket'),
    ('vest-panel-studs', 'vest', 'brass studs along each front edge (~3 a side)', 'vest.studs'),
    ('vest-open-front', 'vest', 'open front: the shirt placket shows between the panels', 'vest.panels'),
    ('pendant-claw', 'pendant', 'black curled claw / hook pendant at the sternum', 'pendant.claw'),
    ('sheath-right-hip', 'sheath', 'ochre tan curved sheath hanging from the belt at the wearer\'s right hip', 'sheath.body'),
    ('sheath-strap-stud', 'sheath', 'belt strap with a stud at the top of the sheath', 'sheath.strap'),
    ('boot-laces', 'boots', 'black criss-cross laces up the front of each boot', 'boots.laces'),
    ('boot-eyelets', 'boots', 'brass eyelets in two columns along the lace panel', 'boots.eyelets'),
    ('boot-toe-sole', 'boots', 'bulbous toe box and a thick sole', 'boots.sole'),
    ('cuff-roll-button', 'cuffs', 'rolled sleeve band with a strap tab and brass button on the outer side', 'cuffs.tab'),
]
p['detailInventory']['scanMethod'] = 'component-zones'
p['detailInventory']['details'] = [
    {'id': d, 'zone': z, 'description': t, 'mapsTo': m, 'confidence': 0.7} for d, z, t, m in details]
p['specDepthDecision']['rationale'] = ('Complex: multiple macro garments with meso pockets / lace panels and micro '
                                       'repeated rivets / eyelets; multiple review views (front, side) needed.')
json.dump(a, open('assessment.json', 'w', encoding='utf-8'), indent=2, ensure_ascii=False)
print('ok', len(p['detailInventory']['details']), 'details')
