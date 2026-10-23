"""Collects the per-crop finish analysis (analyze_texture.py) into material-analysis.json (single-crop route)."""
import glob, json, os

out = {'route': 'single-crop', 'crops': {}}
for f in sorted(glob.glob('material-evidence/*.texture.json')):
    m = os.path.basename(f).replace('.texture.json', '')
    d = json.load(open(f))
    out['crops'][m] = {'crop': f'material-evidence/{m}.png', 'finishClass': d['finishClass'], 'stats': d['stats'],
                       'verifiedOnPart': True}
out['notes'] = ('Crops checked on material-evidence/crops.png (brass, lace, pendant re-cut onto their parts). Flat cel '
                'paint: textureless declared per material; colour recipes from extract_part_color_recipe.py; no PBR '
                'maps (the game shader lights the pieces).')
json.dump(out, open('material-analysis.json', 'w'), indent=1)
print(len(out['crops']), 'crops')
