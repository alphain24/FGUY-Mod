"""Authors object-sculpt-spec.json's geometry for Peter's adventurer outfit (img2threejs spec-authoring).

Every size comes from measure.json (Peter's bind mesh, measured by measure_peter.py) and the reference.
Frame: Peter's bind space - Y up, +Z his front, his right = -X, units = the game's (he is 1.886 tall).
Each piece is fitted where it is worn; all pivots sit at the origin so local = Peter's space."""
import json, math, os, sys

SK = os.path.expanduser('~/.claude/skills/img2threejs')
sys.path.insert(0, os.path.join(SK, 'forge', 'stage2_spec'))
from new_sculpt_spec import _cnode  # noqa: E402  (schema-complete component nodes)
import sdf_eval  # noqa: E402  (the factory's SDF, evaluated here to seat parts on the built surface)

spec = json.load(open('object-sculpt-spec.json', encoding='utf-8'))
M = json.load(open('measure.json'))

# ---- materials: flat cel paint (the game's toon shader draws the light and the ink lines).
# Colours are the lit tone measured from each crop (extract_part_color_recipe.py, material-evidence/*.png);
# the shadow tone is left to the game's shader. Flaps are lifted a step over the panel because our pieces
# get no ink lines in the game, and the pockets must still read.
MAT = {  # id: (lit rgb, shadow rgb, name, roughness, materialClass)
    'hat-felt': ((98, 62, 58), (65, 46, 45), 'Hat felt', 0.9, 'fabric'),
    'hat-band': ((79, 94, 115), (64, 70, 81), 'Hat band cloth', 0.85, 'fabric'),
    'vest-leather': ((98, 63, 59), (58, 42, 39), 'Vest leather', 0.7, 'fabric'),
    'vest-flap': ((114, 74, 68), (94, 61, 57), 'Vest pocket flap leather', 0.7, 'fabric'),
    'brass': ((145, 109, 44), (133, 95, 23), 'Brass hardware', 0.4, 'metal'),
    'boot-leather': ((72, 45, 43), (51, 37, 35), 'Boot leather', 0.7, 'fabric'),
    'lace': ((24, 18, 18), (2, 2, 1), 'Boot laces', 0.9, 'fabric'),
    'sheath-leather': ((128, 88, 22), (94, 63, 16), 'Sheath leather', 0.75, 'fabric'),
    'pendant': ((28, 22, 20), (22, 16, 14), 'Claw pendant', 0.35, 'ceramic'),
    'cuff': ((127, 160, 109), (96, 124, 82), 'Rolled shirt cuff', 0.9, 'fabric'),
}
TEX = {m: json.load(open(f'material-evidence/{m}.texture.json')) for m in MAT}


def hexof(c):
    return '#%02X%02X%02X' % c


base = spec['materials'][0]
mats = []
RECIPES = {}
for mid, (lit, shade, name, rough, cls) in MAT.items():
    m = json.loads(json.dumps(base))
    for f in ('normal', 'bump', 'displacement', 'surfaceFrequencyBands', 'textureProjection', 'textureResolution',
              'referencePbr'):
        m.pop(f, None)
    m.update({'id': mid, 'name': name, 'baseColor': hexof(lit), 'color': hexof(lit)})
    m['albedo'] = {'dominant': hexof(lit), 'secondary': [hexof(shade)], 'samplingNotes':
                   f'Lit / shadow cel tones measured from material-evidence/{mid}.png.'}
    m['colorVariation'] = {'palette': [hexof(lit), hexof(shade)], 'pattern': 'cel two-tone (lit / shadow)',
                           'amplitude': 0.0, 'heightCorrelation': 0.0}
    m['roughness'] = {'base': rough, 'variation': 0.0, 'map': 'none',
                      'localResponse': 'uniform: the game draws cel shading, no roughness maps reach it'}
    m['metalness'] = {'base': 0.3 if mid == 'brass' else 0.0, 'variation': 0.0}
    if mid in ('vest-leather', 'vest-flap', 'boot-leather', 'brass'):
        # the reference paints a lighter highlight band on rounded leather / brass (crop gradientStrength
        # 0.04-0.12): a small roughness variation gives the review render the same satin read
        m['roughness']['variation'] = 0.1
        m['roughness']['localResponse'] = 'satin highlight band on the rounded faces, as painted in the reference'
    st = TEX[mid]['stats']
    m['textureless'] = {'declared': True, 'evidence': [
        f'material-evidence/{mid}.texture.json: mottle {st["mottle"]}, specularFraction {st["specularFraction"]}, '
        f'finishClass {TEX[mid]["finishClass"]} - flat cartoon paint, no grain, print or pores']}
    m['notes'] = ('Stylized cartoon target: the Skin Studio bakes this colour into the model pack\'s palette texture; '
                  'the game\'s own toon shader lights it, so no texture maps are authored.')
    m['localOverrides'] = []
    RECIPES[mid] = {'dominantAlbedo': 'rgba(%d, %d, %d, 1.0)' % lit, 'secondaryAlbedo': 'rgba(%d, %d, %d, 1.0)' % shade,
                   'materialClass': cls, 'materialClassConfidence': 0.7,
                   'evidenceRefs': [f'material-evidence/{mid}.png']}
    mats.append(m)
spec['materials'] = mats
mat_index = {m['id']: m for m in mats}
mat_index['vest-flap']['localOverrides'] = [{'id': 'flap-edge-seam', 'region': 'pocket flap lower edge',
                                             'property': 'color', 'value': '#5C342C',
                                             'notes': 'darker seam line along each flap edge'}]

C = []  # component nodes
NAMES = set()
ZONES = {'hat-zone': ((0.1, 0.0, 0.7, 0.22), 'hat crown, brim and band'),
         'torso-zone': ((0.0, 0.3, 1.0, 0.38), 'vest, pockets, pendant, sleeve cuffs'),
         'hip-zone': ((0.08, 0.64, 0.3, 0.24), 'sheath at the right hip'),
         'boots-zone': ((0.15, 0.76, 0.8, 0.24), 'boots, laces, eyelets')}
spec['viewEvidence'] = spec['viewEvidence'][:1] + [
    {'id': z, 'view': 'primary', 'imageRegion': {'x': r[0], 'y': r[1], 'width': r[2], 'height': r[3],
                                                 'units': 'normalized'}, 'observations': [t], 'confidence': 0.8}
    for z, (r, t) in ZONES.items()]


def zone_of(cid):
    if cid.startswith('hat'): return 'hat-zone'
    if cid.startswith('boot'): return 'boots-zone'
    if cid.startswith('sheath'): return 'hip-zone'
    return 'torso-zone'




def node(cid, name, prim, parent, mat, level='meso', role='body', importance=0.7, topo='assembled-solid',
         rationale=None, pos=(0, 0, 0), rot=(0, 0, 0), scale=(1, 1, 1), desc=None, sdf=None, feats=None,
         evidence=None, socket=None):
    if name in NAMES:                     # every part needs its own name: the viewer selects parts by name
        name = f'{name} {cid}'
    NAMES.add(name)
    n = _cnode(cid, name, prim, parent, list(pos), list(scale), material=mat, role=role, level=level,
               rotation=list(rot), importance=importance, topology_class=topo, topology_rationale=rationale,
               sdf=sdf, local_features=feats or [], evidence=evidence or ['full-object', zone_of(cid)])
    if desc:
        n['geometryDescriptor'].update(desc)
    n['colorMaterialRecipe'] = dict(RECIPES[mat])
    if socket:
        n['actionProfile']['sockets'] = [socket]
    n['fidelityTier'] = 'form'
    C.append(n)
    return n


def feat(fid, kind, text):
    return {'id': fid, 'kind': kind, 'description': text}


# ---- the vest: a shell over Peter's torso. Two ellipsoids fit his measured sections (belly: half-width .52,
# half-depth .51 about z .04 at y .85; chest: half-depth .34 about z .035 at y 1.25), outer = +0.05 all round.
BELLY_C, BELLY_R = (0.0, 0.84, 0.04), (0.525, 0.42, 0.515)
CHEST_C, CHEST_R = (0.0, 1.14, 0.035), (0.46, 0.22, 0.40)
T = 0.05


def grow(r, d):
    return [round(v + d, 4) for v in r]


def front_z(x, y, d=T):
    """The vest's outer surface z at (x, y) on its front (max of the two ellipsoids)."""
    best = -9
    for c, r in ((BELLY_C, BELLY_R), (CHEST_C, CHEST_R)):
        r = grow(r, d)
        k = 1 - (x / r[0]) ** 2 - ((y - c[1]) / r[1]) ** 2
        if k > 0:
            best = max(best, c[2] + r[2] * math.sqrt(k))
    return best


def yaw_at(x, y):
    """Rotation about Y that faces a flat part along the surface normal at (x, y)."""
    dz = front_z(x + 0.01, y) - front_z(x - 0.01, y)
    return math.atan2(-dz / 0.02, 1.0) * -1


def euler_y_to(d):
    """XYZ Euler angles turning +Y onto the unit vector d (a capsule's axis)."""
    y = (0.0, 1.0, 0.0)
    ax = (y[1] * d[2] - y[2] * d[1], y[2] * d[0] - y[0] * d[2], y[0] * d[1] - y[1] * d[0])
    s_ = math.sqrt(sum(v * v for v in ax)); c = sum(a_ * b_ for a_, b_ in zip(y, d))
    if s_ < 1e-9:
        return [0.0, 0.0, 0.0] if c > 0 else [math.pi, 0.0, 0.0]
    k = [v / s_ for v in ax]; ang = math.atan2(s_, c)
    cs, sn, t = math.cos(ang), math.sin(ang), 1 - math.cos(ang)
    m = [[t * k[0] * k[0] + cs, t * k[0] * k[1] - sn * k[2], t * k[0] * k[2] + sn * k[1]],
         [t * k[0] * k[1] + sn * k[2], t * k[1] * k[1] + cs, t * k[1] * k[2] - sn * k[0]],
         [t * k[0] * k[2] - sn * k[1], t * k[1] * k[2] + sn * k[0], t * k[2] * k[2] + cs]]
    ry = math.asin(max(-1.0, min(1.0, m[0][2])))
    if abs(m[0][2]) < 0.9999:
        rx, rz = math.atan2(-m[1][2], m[2][2]), math.atan2(-m[0][1], m[0][0])
    else:
        rx, rz = math.atan2(m[2][1], m[1][1]), 0.0
    return [round(rx, 4), round(ry, 4), round(rz, 4)]


def capsule_chain(prefix, pts, r, closed=False):
    """Capsules joining consecutive points: a rounded trim (rolled edge) along a polyline."""
    out = []
    n = len(pts) if closed else len(pts) - 1
    for k in range(n):
        p0, p1 = pts[k], pts[(k + 1) % len(pts)]
        c = [round((a_ + b_) / 2, 4) for a_, b_ in zip(p0, p1)]
        d = [b_ - a_ for a_, b_ in zip(p0, p1)]
        ln = math.sqrt(sum(v * v for v in d))
        out.append({'id': f'{prefix}{k}', 'type': 'capsule', 'center': c, 'radius': r, 'height': round(ln, 4),
                    'transform': {'position': c, 'rotation': euler_y_to([v / ln for v in d])}})
    return out


def chain_ops(base, prims, radius, out_id):
    """The trim's capsules joined with a near-hard union (a smooth blend at every joint beads the trim),
    then blended onto `base` once with `radius`; the result is named out_id."""
    ops, prev = [], prims[0]['id']
    for k, p in enumerate(prims[1:]):
        oid = f'{out_id}-t{k}'
        ops.append({'id': oid, 'type': 'smooth-union', 'left': prev, 'right': p['id'], 'radius': 0.001})
        prev = oid
    ops.append({'id': out_id, 'type': 'smooth-union', 'left': base, 'right': prev, 'radius': radius})
    return ops


def shell_mid(x, y, side=1):
    """z of the vest shell's mid-thickness at (x, y) on the front (side 1) or back (side -1)."""
    best = None
    for c, r in ((BELLY_C, BELLY_R), (CHEST_C, CHEST_R)):
        r = grow(r, T / 2)
        k = 1 - (x / r[0]) ** 2 - ((y - c[1]) / r[1]) ** 2
        if k > 0:
            z = c[2] + side * r[2] * math.sqrt(k)
            best = z if best is None else (max(best, z) if side > 0 else min(best, z))
    return best


def shell_x(y, z):
    """|x| of the vest shell's mid-thickness at (y, z) (the side seam / armhole edge)."""
    best = 0.0
    for c, r in ((BELLY_C, BELLY_R), (CHEST_C, CHEST_R)):
        r = grow(r, T / 2)
        k = 1 - ((y - c[1]) / r[1]) ** 2 - ((z - c[2]) / r[2]) ** 2
        if k > 0:
            best = max(best, r[0] * math.sqrt(k))
    return best


GAP_BOTTOM, GAP_TOP, GAP_Y0, GAP_Y1, GAP_W = 0.18, 0.30, 0.6, 1.32, 0.2


def gap_side(gid, sx):
    """A box whose outer face runs along the opening's edge from (GAP_BOTTOM, GAP_Y0) to (GAP_TOP, GAP_Y1)."""
    dx, dy = GAP_TOP - GAP_BOTTOM, GAP_Y1 - GAP_Y0
    ln = math.hypot(dx, dy)
    a = -sx * math.asin(dx / ln)                      # lean the box so its face follows the edge
    mx, my = sx * (GAP_BOTTOM + GAP_TOP) / 2, (GAP_Y0 + GAP_Y1) / 2
    c = [round(mx - sx * GAP_W / 2 * math.cos(a), 4), round(my - sx * GAP_W / 2 * math.sin(a), 4), 0.5]
    return {'id': gid, 'type': 'box', 'center': c, 'size': [GAP_W, round(ln + 0.2, 4), 0.5],
            'transform': {'position': c, 'rotation': [0.0, 0.0, round(a, 4)]}}


TRIM_R = 0.034
HEM_Y = 0.645
v_edges, hem, holes = [], [], []
for sx in (1, -1):
    pts = []
    for k in range(7):                     # the opening's edge, hem to collar, on the shell's mid-thickness
        t = k / 6
        y = GAP_Y0 + 0.04 + (1.27 - GAP_Y0 - 0.04) * t
        x = sx * (GAP_BOTTOM + (GAP_TOP - GAP_BOTTOM) * (y - GAP_Y0) / (GAP_Y1 - GAP_Y0) + 0.005)
        pts.append((x, y, shell_mid(x, y)))
    v_edges += capsule_chain(f'vedge{"l" if sx > 0 else "r"}', pts, TRIM_R)
    ring = []
    for k in range(8):                     # the armhole: around the arm capsule's axis where it meets the shell
        th = 2 * math.pi * k / 8
        y, z = 1.17 + 0.165 * math.cos(th), -0.01 + 0.165 * math.sin(th)
        ring.append((sx * max(0.3, shell_x(y, z)), y, z))
    holes += capsule_chain(f'arm{"l" if sx > 0 else "r"}', ring, TRIM_R * 0.9, closed=True)
pts = []                                   # the hem, round the back from one front edge to the other
HEM_GAP = math.degrees(math.asin(min(1.0, (GAP_BOTTOM + 0.01) / (grow(BELLY_R, T / 2)[0] * 0.75))))
for k in range(17):                        # angle about Y from the front (+Z); skip only the opening
    th = math.radians(HEM_GAP + (360 - 2 * HEM_GAP) * k / 16)
    r = grow(BELLY_R, T / 2)
    f = math.sqrt(max(0.0, 1 - ((HEM_Y - BELLY_C[1]) / r[1]) ** 2))
    pts.append((r[0] * f * math.sin(th), HEM_Y, BELLY_C[2] + r[2] * f * math.cos(th)))
hem = capsule_chain('hem', pts, TRIM_R)
VEST_TRIM = v_edges + hem + holes
VEST_TRIM_OPS = chain_ops('vest-cut', VEST_TRIM, 0.03, 'vest')

vest_sdf = {
    'primitives': [
        {'id': 'belly-out', 'type': 'ellipsoid', 'center': list(BELLY_C), 'radii': grow(BELLY_R, T)},
        {'id': 'chest-out', 'type': 'ellipsoid', 'center': list(CHEST_C), 'radii': grow(CHEST_R, T)},
        {'id': 'belly-in', 'type': 'ellipsoid', 'center': list(BELLY_C), 'radii': grow(BELLY_R, 0.0)},
        {'id': 'chest-in', 'type': 'ellipsoid', 'center': list(CHEST_C), 'radii': grow(CHEST_R, 0.0)},
        # open front between the panels (the shirt placket shows: reference x 300-400 of 540)
        # open front: a V, ~0.36 wide at the belt widening to ~0.6 at the collar (reference: the shirt shows
        # over ~40% of the torso at the belt) - a centre strip plus one tilted box per side
        {'id': 'front-gap', 'type': 'box', 'center': [0.0, 0.98, 0.5], 'size': [0.36, 0.78, 0.5]},
        gap_side('gap-l', 1), gap_side('gap-r', -1),
        {'id': 'neck', 'type': 'ellipsoid', 'center': [0.0, 1.37, 0.1], 'radii': [0.27, 0.17, 0.34]},
        {'id': 'arm-l', 'type': 'capsule', 'center': [0.62, 1.17, -0.01], 'radius': 0.175, 'height': 0.5,
         'transform': {'position': [0.62, 1.17, -0.01], 'rotation': [0.0, 0.0, 1.5708]}},
        {'id': 'arm-r', 'type': 'capsule', 'center': [-0.62, 1.17, -0.01], 'radius': 0.175, 'height': 0.5,
         'transform': {'position': [-0.62, 1.17, -0.01], 'rotation': [0.0, 0.0, 1.5708]}},
        {'id': 'below-belt', 'type': 'box', 'center': [0.0, 0.3, 0.0], 'size': [2.0, 0.66, 2.0]},
    ] + VEST_TRIM,
    'operations': [
        {'id': 'outer', 'type': 'smooth-union', 'left': 'belly-out', 'right': 'chest-out', 'radius': 0.12},
        {'id': 'inner', 'type': 'smooth-union', 'left': 'belly-in', 'right': 'chest-in', 'radius': 0.12},
        {'id': 'shell', 'type': 'subtract', 'left': 'outer', 'right': 'inner'},
        {'id': 'gap1', 'type': 'smooth-union', 'left': 'front-gap', 'right': 'gap-l', 'radius': 0.02},
        {'id': 'gap2', 'type': 'smooth-union', 'left': 'gap1', 'right': 'gap-r', 'radius': 0.02},
        {'id': 'open', 'type': 'subtract', 'left': 'shell', 'right': 'gap2'},
        {'id': 'necked', 'type': 'subtract', 'left': 'open', 'right': 'neck'},
        {'id': 'arm1', 'type': 'subtract', 'left': 'necked', 'right': 'arm-l'},
        {'id': 'arm2', 'type': 'subtract', 'left': 'arm1', 'right': 'arm-r'},
        {'id': 'vest-cut', 'type': 'subtract', 'left': 'arm2', 'right': 'below-belt'},
    ] + VEST_TRIM_OPS,
    'bounds': {'min': [-0.62, 0.6, -0.54], 'max': [0.62, 1.42, 0.64]},
    'resolution': 64,
}
node('vest-shell', 'Leather vest shell', 'ellipsoid', None, 'vest-leather', level='macro', importance=1.0,
     topo='implicit', sdf=vest_sdf,
     rationale='Continuous garment shell offset 0.05 outward from the wearer\'s measured torso surface (belly and '
               'chest ellipsoids), hollowed, with the open front, neck, armholes and hem cut away.',
     feats=[feat('vest-open-front', 'seam', 'open front gap 0.26 wide, the shirt shows'),
            feat('vest-armholes', 'hole', 'armholes around the upper arms')])

# pockets: rounded-corner patches extruded off the panel along its surface normal (their back sunk 0.008 into the
# leather), flaps a step lighter and proud of the pocket top; each sits between the opening's edge and the side
POCKETS = [('chest', 0.37, 1.0, 0.16, 0.13), ('lower', 0.36, 0.77, 0.25, 0.2)]


VEST_OUTER = sdf_eval.field(vest_sdf, until='outer')   # the vest's outer surface, before any cut or trim


def surface_frame(x, y):
    """The vest's outer front surface at (x, y) as the factory builds it: the point, its normal, and the Euler XYZ
    turning local +Z onto that normal."""
    p = sdf_eval.surface(VEST_OUTER, [x, y, 0.04], [0, 0, 1])
    n = sdf_eval.normal(VEST_OUTER, p)
    return tuple(p), n, [round(math.atan2(-n[1], n[2]), 4), round(math.asin(max(-1.0, min(1.0, n[0]))), 4), 0.0]


def rounded_rect(w, h, r, cy=0.0, taper=0.0):
    """Outline of a w x h rectangle with corner radius r (optionally narrower by `taper` at the bottom)."""
    pts = []
    for cx_, cy_, a0 in ((w / 2 - r, h / 2 - r, 0), (-w / 2 + r, h / 2 - r, 90), (-w / 2 + r + taper, -h / 2 + r, 180),
                         (w / 2 - r - taper, -h / 2 + r, 270)):
        for k in range(4):
            a = math.radians(a0 + 90 * k / 3)
            pts.append([round(cx_ + r * math.cos(a), 4), round(cy + cy_ + r * math.sin(a), 4)])
    return pts


def on_surface(p, n, d):
    return tuple(round(p[i] + n[i] * d, 4) for i in range(3))


buttons = []
for kind, px, py, w, h in POCKETS:
    for side, sx in (('l', 1), ('r', -1)):
        p, n, rot = surface_frame(sx * px, py)
        node(f'vest-{kind}-pocket-{side}', f'Vest {kind} pocket ({"left" if sx > 0 else "right"})', 'extrude',
             'vest-shell', 'vest-leather', importance=0.8, pos=on_surface(p, n, -0.008), rot=rot,
             desc={'profile2D': {'points': rounded_rect(w, h, 0.02), 'depth': 0.03}},
             rationale='A patch pocket sewn onto the vest panel: its rounded outline extruded to the leather\'s '
                       'thickness along the panel normal.',
             feats=[feat(f'vest-{kind}-pocket-flap-{side}', 'seam', f'{kind} pocket body under its flap')])
        fy = py + h / 2 - 0.03
        fp, fn, frot = surface_frame(sx * px, fy)
        fh = 0.07 if kind == 'chest' else 0.08
        node(f'vest-{kind}-flap-{side}', f'Vest {kind} pocket flap ({side})', 'extrude', 'vest-shell', 'vest-flap',
             importance=0.8, pos=on_surface(fp, fn, 0.012), rot=frot,
             desc={'profile2D': {'points': rounded_rect(w + 0.02, fh, 0.015, cy=-fh / 2 + 0.02, taper=0.01),
                                 'depth': 0.025}},
             rationale='The flap over the pocket mouth: a slightly tapered outline extruded proud of the pocket.',
             feats=[feat(f'flap-seam-{kind}-{side}', 'seam', 'flap lower edge seam')])
        bp, bn, _ = surface_frame(sx * px, fy - fh + 0.035)
        buttons.append((f'vest-{kind}-button-{side}', on_surface(bp, bn, 0.04)))
# panel studs just inside each front edge (3 a side)
for side, sx in (('l', 1), ('r', -1)):
    for k, y in enumerate((0.72, 0.86, 1.0)):
        edge = GAP_BOTTOM + (GAP_TOP - GAP_BOTTOM) * (y - GAP_Y0) / (GAP_Y1 - GAP_Y0)
        bp, bn, _ = surface_frame(sx * (edge + 0.06), y)
        buttons.append((f'vest-stud-{side}{k}', on_surface(bp, bn, 0.005)))
for bid, pos in buttons:
    node(bid, 'Brass button / stud', 'sphere', 'vest-shell', 'brass', level='micro', importance=0.5,
         pos=pos, scale=(0.035, 0.035, 0.022),
         rationale='A small domed brass fastener: a discrete squashed sphere.',
         feats=[feat(bid + '-dome', 'fastener', 'domed brass head')])

# ---- claw pendant: a tapered sweep curling into a hook below the collar (chest front z .40 at y 1.2)
PZ = 0.43
# a crescent claw: from the bail at its upper left it sweeps down round the bottom and up to a tip at the upper
# right (counter-clockwise from 150 to 400 degrees), its radius and thickness falling toward the tip
CLAW_C, CLAW = (0.015, 1.165), []
for k in range(13):
    t = k / 12
    th = math.radians(150 + 250 * t)
    rr = 0.05 - 0.015 * t
    CLAW.append({'position': [round(CLAW_C[0] + rr * math.cos(th), 4), round(CLAW_C[1] + rr * math.sin(th), 4),
                              round(PZ + 0.012 * math.sin(math.pi * t), 4)],
                 'rx': round(0.026 * (1 - t) + 0.002, 4), 'rz': round(0.017 * (1 - t) + 0.002, 4)})
node('pendant-claw', 'Black claw pendant', 'tapered-sweep', 'vest-shell', 'pendant', importance=0.85,
     topo='fiber-strand', rationale='A single tapering curved horn/claw: a swept tube whose radius falls to a point.',
     desc={'taperedSweep': {'stations': CLAW, 'radialSegments': 10, 'capEnds': True}},
     feats=[feat('pendant-claw-hook', 'ridge', 'curled hook tip')])
BAIL = CLAW[0]['position']
node('pendant-bail', 'Pendant bail', 'torus', 'pendant-claw', 'pendant', level='micro', importance=0.4,
     pos=(BAIL[0] - 0.005, BAIL[1] + 0.03, BAIL[2] - 0.004), rot=(0, 0, 0), scale=(0.045, 0.045, 0.045),
     desc={'torusTubeRatio': 0.3},
     rationale='The small ring the claw hangs from: a torus.', feats=[feat('pendant-ring', 'ridge', 'hanging ring')])

# ---- hat: crown + brim on the head (head top y 1.886; at y 1.80 the head is x +-.233, z -.154..+.249)
HX, HZ = 0.0, 0.05
BRIM_Y = 1.82
# brim: constant-thickness slabs (a tapering ellipsoid gets thinner than the sampling grid at its rim and tears),
# trimmed to the brim's elliptic outline; the sides hinge up at |x| = BRIM_HINGE (the reference's rolled sides)
BRIM_T, BRIM_HINGE, CURL, SIDE_L = 0.038, 0.24, 0.6, 0.3
OUTLINE = [0.43, 2.0, 0.47]


def side_slab(sid, sx):
    c = [round(HX + sx * (BRIM_HINGE + math.cos(CURL) * SIDE_L / 2), 4), round(BRIM_Y + math.sin(CURL) * SIDE_L / 2, 4), HZ]
    return {'id': sid, 'type': 'box', 'center': c, 'size': [SIDE_L, BRIM_T, 2.0],
            'transform': {'position': c, 'rotation': [0.0, 0.0, round(sx * CURL, 4)]}}


RIM_R, RIM_N = 0.028, 18


def rim_point(th):
    x = HX + (OUTLINE[0] - RIM_R * 0.6) * math.cos(th)
    z = HZ + 0.02 + (OUTLINE[2] - RIM_R * 0.6) * math.sin(th)
    lift = max(0.0, abs(x - HX) - BRIM_HINGE) * math.tan(CURL)
    return (x, BRIM_Y + lift, z)


RIM = []
for k in range(RIM_N):
    p0, p1 = rim_point(2 * math.pi * k / RIM_N), rim_point(2 * math.pi * (k + 1) / RIM_N)
    c = [round((a_ + b_) / 2, 4) for a_, b_ in zip(p0, p1)]
    d = [b_ - a_ for a_, b_ in zip(p0, p1)]
    ln = math.sqrt(sum(v * v for v in d))
    RIM.append({'id': f'rim{k}', 'type': 'capsule', 'center': c, 'radius': RIM_R, 'height': round(ln, 4),
                'transform': {'position': c, 'rotation': euler_y_to([v / ln for v in d])}})

hat_sdf = {
    'primitives': [
        {'id': 'crown', 'type': 'ellipsoid', 'center': [HX, BRIM_Y + 0.1, HZ], 'radii': [0.25, 0.24, 0.28]},
        {'id': 'crown-top', 'type': 'ellipsoid', 'center': [HX, BRIM_Y + 0.22, HZ], 'radii': [0.22, 0.12, 0.26]},
        {'id': 'crease', 'type': 'capsule', 'center': [HX, BRIM_Y + 0.355, HZ], 'radius': 0.04, 'height': 0.32,
         'transform': {'position': [HX, BRIM_Y + 0.355, HZ], 'rotation': [1.5708, 0.0, 0.0]}},
        {'id': 'outline', 'type': 'ellipsoid', 'center': [HX, BRIM_Y, HZ + 0.02], 'radii': OUTLINE},
        {'id': 'brim-mid', 'type': 'box', 'center': [HX, BRIM_Y, HZ], 'size': [2 * BRIM_HINGE, BRIM_T, 2.0]},
        side_slab('brim-l', 1), side_slab('brim-r', -1),
        {'id': 'below', 'type': 'box', 'center': [HX, BRIM_Y - 0.25, HZ], 'size': [1.2, 0.4, 1.2]},
    ] + RIM,
    'operations': [
        {'id': 'crown2', 'type': 'smooth-union', 'left': 'crown', 'right': 'crown-top', 'radius': 0.06},
        {'id': 'creased', 'type': 'subtract', 'left': 'crown2', 'right': 'crease'},
        {'id': 'mid', 'type': 'intersect', 'left': 'brim-mid', 'right': 'outline'},
        {'id': 'side1', 'type': 'intersect', 'left': 'brim-l', 'right': 'outline'},
        {'id': 'side2', 'type': 'intersect', 'left': 'brim-r', 'right': 'outline'},
        {'id': 'brim1', 'type': 'smooth-union', 'left': 'mid', 'right': 'side1', 'radius': 0.05},
        {'id': 'brim2', 'type': 'smooth-union', 'left': 'brim1', 'right': 'side2', 'radius': 0.05},
    ] + [{'id': f'rimmed{k}', 'type': 'smooth-union', 'left': 'brim2' if k == 0 else f'rimmed{k - 1}',
          'right': f'rim{k}', 'radius': 0.02} for k in range(RIM_N)] + [
        {'id': 'hat', 'type': 'smooth-union', 'left': 'creased', 'right': f'rimmed{RIM_N - 1}', 'radius': 0.03},
        {'id': 'hat-cut', 'type': 'subtract', 'left': 'hat', 'right': 'below'},
    ],
    'bounds': {'min': [-0.5, BRIM_Y - 0.06, HZ - 0.5], 'max': [0.5, BRIM_Y + 0.4, HZ + 0.52]},
    'resolution': 64,
}
node('hat-crown', 'Wide-brim hat (crown and brim)', 'ellipsoid', 'vest-shell', 'hat-felt', level='macro',
     importance=1.0, topo='implicit', sdf=hat_sdf,
     rationale='Felt hat: domed crown with a pinch crease blended into a brim whose lateral sides roll up; one '
               'continuous implicit form.',
     feats=[feat('hat-brim-curl', 'ridge', 'brim sides rolled up 0.55 rad'),
            feat('hat-crown-crease', 'groove', 'front-to-back crease on the crown top')])
# band: on the crown above the brim fillet (crown radii there x .248, z .278); its centreline 0.005 outside the
# felt, a 0.025-thick ribbon 0.054 tall (torus 0.45 / tube 0.045, scaled, then turned flat)
BAND_Y = BRIM_Y + 0.07
node('hat-band', 'Hat band', 'torus', 'hat-crown', 'hat-band', importance=0.9, pos=(HX, BAND_Y, HZ),
     rot=(1.5708, 0, 0), scale=(round(0.253 / 0.45, 4), round(0.283 / 0.45, 4), 0.6), desc={'torusTubeRatio': 0.1},
     rationale='A cloth ring wrapping the crown base: an oval torus slightly proud of the felt.',
     feats=[feat('hat-band-ring', 'ridge', 'blue band ring')])

# ---- boots: per leg (leg centre x +-.19, z .05; half x .17, half z .21; shoe toe z .376, heel z -.156)
BOOT_TOP = 0.29   # (below his knee - at 0.32: a boot over the knee lets the trousers bulge out of it as it bends)
for side, sx in (('l', 1), ('r', -1)):
    # shaft a touch wider than his trouser leg (half x .17, half z .21 at y .35) so the leg never shows through
    cx, SHAFT_R = sx * 0.217, (0.2, 0.33, 0.265)   # (each covers its leg - inner side x .02, the collar the rest - never meeting)
    kk = math.sqrt(1 - ((BOOT_TOP - 0.012 - 0.22) / SHAFT_R[1]) ** 2)
    collar = capsule_chain('collar', [(cx + SHAFT_R[0] * kk * math.cos(2 * math.pi * k / 16), BOOT_TOP - 0.012,
                                       0.04 + (SHAFT_R[2] * kk + 0.004) * math.sin(2 * math.pi * k / 16))
                                      for k in range(16)], 0.022, closed=True)
    sdf = {
        'primitives': [
            {'id': 'shaft', 'type': 'ellipsoid', 'center': [cx, 0.22, 0.04], 'radii': list(SHAFT_R)},
            {'id': 'toe', 'type': 'ellipsoid', 'center': [cx, 0.1, 0.19], 'radii': [0.195, 0.115, 0.255]},
            # (the sole 5 mm below the ground line: the character's own soles, at -0.0015, never show under it)
            # sole: the boot's own footprint (an oval a touch wider than the foot) cut to a 0.06 slab
            {'id': 'sole-print', 'type': 'ellipsoid', 'center': [cx, 0.03, 0.12], 'radii': [0.185, 0.6, 0.31]},
            {'id': 'sole-slab', 'type': 'box', 'center': [cx, 0.025, 0.12], 'size': [1.0, 0.06, 1.0]},
            {'id': 'top', 'type': 'box', 'center': [cx, BOOT_TOP + 0.3, 0.1], 'size': [0.8, 0.6, 1.2]},
            {'id': 'under', 'type': 'box', 'center': [cx, -0.305, 0.1], 'size': [0.8, 0.6, 1.2]},
        ] + collar,
        'operations': [
            {'id': 'foot', 'type': 'smooth-union', 'left': 'shaft', 'right': 'toe', 'radius': 0.1},
            {'id': 'sole', 'type': 'intersect', 'left': 'sole-print', 'right': 'sole-slab'},
            {'id': 'soled', 'type': 'smooth-union', 'left': 'foot', 'right': 'sole', 'radius': 0.03},
            {'id': 'cut', 'type': 'subtract', 'left': 'soled', 'right': 'top'},
            {'id': 'boot-cut', 'type': 'subtract', 'left': 'cut', 'right': 'under'},
        ] + chain_ops('boot-cut', collar, 0.03, 'boot'),
        'bounds': {'min': [cx - 0.25, -0.015, -0.25], 'max': [cx + 0.25, BOOT_TOP + 0.03, 0.48]},
        'resolution': 64,
    }
    node(f'boot-shell-{side}', f'Lace-up boot ({"left" if sx > 0 else "right"})', 'ellipsoid', 'vest-shell',
         'boot-leather', level='macro', importance=0.95, topo='implicit', sdf=sdf,
         rationale='Boot over the shoe and lower leg: shaft and bulbous toe box blended over a flat sole, top cut '
                   'at mid-shin.',
         feats=[feat(f'boot-toe-sole-{side}', 'ridge', 'bulbous toe and thick sole')])

    BOOT_F = sdf_eval.field(sdf)

    def boot_front(y, dx=0.0):
        """z of the boot's built front surface (fillets included) at height y, dx off its centre line."""
        return sdf_eval.surface(BOOT_F, [cx + dx, y, 0.04], [0, 0, 1])[2]

    # four rows of eyelets (the reference's three bold Xs), the laces riding over the curved front: each run from an
    # eyelet up to the middle of the front is lifted off the leather there, lace b crossing over lace a
    ys, LDX = [0.115, 0.158, 0.201, 0.244], 0.056
    for k, y in enumerate(ys):
        for col, dx in (('a', -LDX), ('b', LDX)):
            node(f'boot-eyelet-{side}{k}{col}', 'Boot eyelet', 'sphere', f'boot-shell-{side}', 'brass',
                 level='micro', importance=0.4, pos=(cx + dx, y, round(boot_front(y, dx) + 0.002, 4)),
                 scale=(0.027, 0.027, 0.015),
                 rationale='A small brass grommet: a discrete squashed sphere.',
                 feats=[feat(f'boot-eyelet-{side}{k}{col}-ring', 'fastener', 'brass eyelet')])
    for li, flip, lift in (('a', 1, 0.016), ('b', -1, 0.026)):
        for k in range(len(ys) - 1):
            dx0 = LDX * flip * (1 if k % 2 == 0 else -1)
            y0, y1 = ys[k], ys[k + 1]
            ym = (y0 + y1) / 2
            ends = [[round(cx + dx0, 4), y0, round(boot_front(y0, dx0) + 0.01, 4)],
                    [round(cx - dx0, 4), y1, round(boot_front(y1, -dx0) + 0.01, 4)]]
            mid = [round(cx, 4), round(ym, 4), round(boot_front(ym, 0.0) + lift, 4)]
            for h, (a, b) in enumerate(((ends[0], mid), (mid, ends[1]))):
                # half a lace run: a cord pulled straight from an eyelet to the crossing - an endpoint cylinder
                node(f'boot-lace-{side}{li}{k}{h}', 'Boot lace run', 'cylinder', f'boot-shell-{side}', 'lace',
                     level='micro', importance=0.85, topo='fiber-strand',
                     rationale='A lace run is a thin cord pulled straight from an eyelet to where it crosses the other '
                               'lace: a cylinder spanning them, lifted off the leather at the crossing.',
                     feats=[feat(f'boot-laces-{side}{li}{k}{h}', 'stitch', 'criss-cross lace run')])
                C[-1]['attachment'] = {'parentSocket': f'boot-{side}-eyelets', 'localStart': a, 'localEnd': b,
                                       'contactType': 'threaded-through', 'baseRadius': 0.014, 'endRadius': 0.014,
                                       'embedDepth': 0.004, 'gapTolerance': 0.004,
                                       'evidenceRefs': ['boots-zone']}

# ---- knife sheath on his right hip (-X), hanging off the belt (belt y .55-.65; hip half-width .43 at y .55)
# its broad face points forward-out (-0.82, 0, 0.57): on the hip's front corner, as the reference shows it
SH_YAW, SH_TILT = -0.96, 0.15
SH_N = (math.sin(SH_YAW), 0.0, math.cos(SH_YAW))
SH_P = (-0.381, 0.62, 0.31)
# the sheath: a flattened leather form along a centre line that curves forward as it drops 0.42 from the belt,
# its width tapering to the tip: smooth-unioned flat ellipsoids in the sheath's plane (face normal SH_N)
SH_U = (math.cos(SH_YAW), 0.0, -math.sin(SH_YAW))          # in-plane horizontal
SH_LEN, SH_N_ELL = 0.42, 9


def sheath_point(t):
    off = -0.06 * t * t                                    # curve toward the front edge (the reference's tip)
    return [SH_P[i] + SH_U[i] * off + (0.0, -SH_LEN * t, 0.0)[i] for i in range(3)]


sheath_prims = []
for k in range(SH_N_ELL):
    t0, t1 = k / SH_N_ELL, (k + 1) / SH_N_ELL
    p0, p1 = sheath_point(t0), sheath_point(t1)
    c = [round((a_ + b_) / 2, 4) for a_, b_ in zip(p0, p1)]
    tm = (t0 + t1) / 2
    w = 0.072 * (1 - 0.88 * tm ** 1.6)
    du = sum((p1[i] - p0[i]) * SH_U[i] for i in range(3))
    tilt = math.atan2(-du, SH_LEN / SH_N_ELL)               # lean each piece along the curve (rotation in-plane)
    sheath_prims.append({'id': f'sh{k}', 'type': 'ellipsoid', 'center': c,
                         'radii': [round(w, 4), round(SH_LEN / SH_N_ELL * 0.85, 4), 0.022],
                         'transform': {'position': c, 'rotation': [0.0, SH_YAW, round(tilt, 4)]}})
sheath_ops = [{'id': f'sho{k}' if k < SH_N_ELL - 1 else 'sheath', 'type': 'smooth-union',
               'left': 'sh0' if k == 1 else f'sho{k - 1}', 'right': f'sh{k}', 'radius': 0.03}
              for k in range(1, SH_N_ELL)]
sheath_ops[-1]['id'] = 'sheath'
node('sheath-body', 'Knife sheath', 'ellipsoid', 'vest-shell', 'sheath-leather', level='macro', importance=0.9,
     topo='implicit',
     sdf={'primitives': sheath_prims, 'operations': sheath_ops, 'resolution': 48,
          'bounds': {'min': [round(SH_P[0] - 0.16, 3), round(SH_P[1] - SH_LEN - 0.05, 3), round(SH_P[2] - 0.16, 3)],
                     'max': [round(SH_P[0] + 0.16, 3), round(SH_P[1] + 0.05, 3), round(SH_P[2] + 0.16, 3)]}},
     rationale='A moulded leather sheath: one continuous flattened form, widest at the mouth, curving and '
               'tapering to its tip.',
     feats=[feat('sheath-right-hip', 'seam', 'curved sheath body tapering to a point')])
node('sheath-strap', 'Sheath belt strap', 'box', 'sheath-body', 'sheath-leather', importance=0.6,
     pos=tuple(p + n * 0.045 for p, n in zip((SH_P[0], 0.6, SH_P[2]), SH_N)), rot=(0, SH_YAW, SH_TILT),
     scale=(0.07, 0.13, 0.025),
     rationale='A strap looping over the belt: a thin assembled block.',
     feats=[feat('sheath-strap-loop', 'seam', 'belt loop strap')])
node('sheath-stud', 'Sheath strap stud', 'sphere', 'sheath-strap', 'brass', level='micro', importance=0.4,
     pos=tuple(p + n * 0.06 for p, n in zip((SH_P[0], 0.58, SH_P[2]), SH_N)), scale=(0.025, 0.025, 0.025),
     rationale='A brass rivet head: a small sphere.', feats=[feat('sheath-strap-stud', 'fastener', 'rivet')])

# ---- rolled sleeve cuffs at the short-sleeve hems (arm along X; centre y 1.17, z -.015; sleeve radius ~.17)
for side, sx in (('l', 1), ('r', -1)):
    x = sx * 0.63
    node(f'sleeve-cuff-{side}', f'Rolled sleeve cuff ({side})', 'torus', 'vest-shell', 'cuff', importance=0.75,
         pos=(x, 1.17, -0.015), rot=(0, sx * 1.5708, 0), scale=(0.37, 0.37, 0.5), desc={'torusTubeRatio': 0.2},
         rationale='A rolled band around the sleeve end: a torus ring.',
         feats=[feat(f'cuff-roll-{side}', 'ridge', 'rolled band')])
    node(f'sleeve-tab-{side}', f'Sleeve strap tab ({side})', 'box', f'sleeve-cuff-{side}', 'cuff', importance=0.5,
         pos=(x - sx * 0.065, 1.17 + 0.158, -0.01), scale=(0.1, 0.018, 0.055),
         rationale='The roll-up strap: a thin tab on the outer side.', feats=[feat(f'cuff-tab-{side}', 'seam', 'tab')])
    node(f'sleeve-button-{side}', f'Sleeve tab button ({side})', 'sphere', f'sleeve-tab-{side}', 'brass',
         level='micro', importance=0.4, pos=(x - sx * 0.09, 1.17 + 0.172, -0.01), scale=(0.03, 0.018, 0.03),
         rationale='A brass button: a squashed sphere.', feats=[feat(f'cuff-button-{side}', 'fastener', 'button')])

def rot_m(e):
    """THREE Euler XYZ -> 3x3 (R = Rx Ry Rz)."""
    cx, sx_, cy, sy, cz, sz = math.cos(e[0]), math.sin(e[0]), math.cos(e[1]), math.sin(e[1]), math.cos(e[2]), math.sin(e[2])
    rx = [[1, 0, 0], [0, cx, -sx_], [0, sx_, cx]]
    ry = [[cy, 0, sy], [0, 1, 0], [-sy, 0, cy]]
    rz = [[cz, -sz, 0], [sz, cz, 0], [0, 0, 1]]
    mul = lambda a_, b_: [[sum(a_[i][k] * b_[k][j] for k in range(3)) for j in range(3)] for i in range(3)]
    return mul(mul(rx, ry), rz)


def m_euler(m):
    ry = math.asin(max(-1.0, min(1.0, m[0][2])))
    if abs(m[0][2]) < 0.9999:
        return [math.atan2(-m[1][2], m[2][2]), ry, math.atan2(-m[0][1], m[0][0])]
    return [math.atan2(m[2][1], m[1][1]), ry, 0.0]


# triangle budget: the game skins every vertex on the CPU each frame, so the outfit stays near Peter's own size
# (8,680 triangles): the implicit shells and the many small spheres / tori are decimated (quadric collapse)
DECIMATE = {'vest-shell': 0.12, 'boot-shell-l': 0.1, 'boot-shell-r': 0.1, 'hat-crown': 0.15, 'sheath-body': 0.25}
for c in C:
    r = DECIMATE.get(c['id']) or {'sphere': 0.06, 'torus': 0.3}.get(c['primitive'] if c['topologyClass'] != 'implicit'
                                                                     else '')
    if r:
        c['geometryDescriptor']['decimate'] = {'targetRatio': r}

world = {c['id']: (c['transform']['position'], c['transform']['rotation']) for c in C}
for c in C:
    if not c['parent'] or c['topologyClass'] == 'implicit' and not any(c['transform']['position']):
        continue
    pp, pr = world[c['parent']]
    if not any(pp) and not any(pr):
        continue
    R = rot_m(pr)
    Rt = [[R[j][i] for j in range(3)] for i in range(3)]
    p, r = world[c['id']]
    d = [p[i] - pp[i] for i in range(3)]
    c['transform']['position'] = [round(sum(Rt[i][k] * d[k] for k in range(3)), 5) for i in range(3)]
    M = rot_m(r)
    L = [[sum(Rt[i][k] * M[k][j] for k in range(3)) for j in range(3)] for i in range(3)]
    c['transform']['rotation'] = [round(v, 5) for v in m_euler(L)]
    if c.get('attachment'):
        for key in ('localStart', 'localEnd'):
            q = [c['attachment'][key][i] - pp[i] for i in range(3)]
            c['attachment'][key] = [round(sum(Rt[i][k] * q[k] for k in range(3)), 5) for i in range(3)]

spec['componentTree'] = C
ids = [c['id'] for c in C]

# repetition systems: documentation of the mirrored / repeated sets (each member is built individually)
spec['repetitionSystems'] = [
    {'id': 'vest-buttons', 'level': 'micro', 'parent': 'vest-shell', 'primitive': 'sphere', 'material': 'brass',
     'count': len(buttons), 'elementComponentIds': [b[0] for b in buttons],
     'placement': {'mode': 'authored', 'notes': 'one per pocket flap + 3 studs down each front edge'}},
    {'id': 'boot-eyelets', 'level': 'micro', 'parent': 'vest-shell', 'primitive': 'sphere', 'material': 'brass',
     'count': 16, 'elementComponentIds': [i for i in ids if i.startswith('boot-eyelet-')],
     'placement': {'mode': 'authored', 'notes': 'two columns of 4 per boot'}},
    {'id': 'mirrored-pairs', 'level': 'meso', 'parent': 'vest-shell', 'primitive': 'box', 'material': 'vest-leather',
     'count': 4, 'elementComponentIds': [i for i in ids if 'pocket' in i],
     'placement': {'mode': 'mirror-x', 'notes': 'pockets mirrored left/right'}},
]

# detail inventory: map onto real component ids
REMAP = {'hat-brim': 'hat-crown', 'hat-crown': 'hat-crown', 'vest-chest-pockets': 'vest-chest-pocket-l',
         'vest-lower-pockets': 'vest-lower-pocket-l', 'vest-studs': 'vest-stud-l0', 'boot-laces': 'boot-lace-la00',
         'boot-eyelets': 'boot-eyelet-l0a', 'boot-shells': 'boot-shell-l', 'sleeve-cuffs': 'sleeve-cuff-l'}
for d in spec['preSpecAssessment']['detailInventory']['details']:
    r = d['mapsTo']['ref']
    d['mapsTo'] = {'type': 'component', 'ref': REMAP.get(r, r)}
    assert d['mapsTo']['ref'] in ids, d['mapsTo']

groups = {
    'blockout': ['vest-shell', 'hat-crown', 'boot-shell-l', 'boot-shell-r', 'sheath-body'],
}
groups['structural-pass'] = groups['blockout'] + [i for i in ids if 'pocket' in i or 'flap' in i or i in
                                                  ('hat-band', 'sleeve-cuff-l', 'sleeve-cuff-r', 'sheath-strap',
                                                   'pendant-claw')]
for p in spec['buildPasses']:
    p['componentRefs'] = groups.get(p['id'], ids)

spec['featureReviewTargets'] = [
    {'id': 'hat-silhouette', 'name': 'Wide brim curled at the sides with the blue band', 'tier': 'critical',
     'passIds': ['blockout', 'structural-pass', 'form-refinement'], 'minimumScore': 0.8, 'mustPass': True,
     'componentRefs': ['hat-crown', 'hat-band'], 'evidenceRefs': ['hat-zone']},
    {'id': 'vest-and-pockets', 'name': 'Dark leather vest, open front, big flap pockets with brass buttons',
     'tier': 'critical', 'passIds': ['blockout', 'structural-pass', 'form-refinement'], 'minimumScore': 0.8,
     'mustPass': True, 'componentRefs': ['vest-shell'] + [i for i in ids if 'pocket' in i or 'flap' in i],
     'evidenceRefs': ['torso-zone']},
    {'id': 'boots-and-laces', 'name': 'Bulbous lace-up boots with black crossed laces', 'tier': 'critical',
     'passIds': ['blockout', 'structural-pass', 'form-refinement'], 'minimumScore': 0.8, 'mustPass': True,
     'componentRefs': ['boot-shell-l', 'boot-shell-r'], 'evidenceRefs': ['boots-zone']},
    {'id': 'sheath-right-hip', 'name': 'Tan curved sheath on the wearer\'s right hip', 'tier': 'important',
     'passIds': ['structural-pass', 'form-refinement'], 'minimumScore': 0.75, 'mustPass': False,
     'componentRefs': ['sheath-body'], 'evidenceRefs': ['hip-zone']},
    {'id': 'pendant-and-cuffs', 'name': 'Black claw pendant and rolled cuffs', 'tier': 'important',
     'passIds': ['structural-pass', 'form-refinement'], 'minimumScore': 0.7, 'mustPass': False,
     'componentRefs': ['pendant-claw', 'sleeve-cuff-l', 'sleeve-cuff-r'], 'evidenceRefs': ['torso-zone']},
    {'id': 'outfit-and-palette', 'name': 'Outfit palette matches the reference', 'tier': 'critical',
     'passIds': ['material-pass'], 'minimumScore': 0.8, 'mustPass': True, 'componentRefs': ids[:1],
     'evidenceRefs': ['full-object']},
]
spec['coordinateFrame'] = {'front': '+Z (Peter\'s face)', 'up': '+Y', 'handedness': 'right-handed',
                           'wearerRight': '-X (image left in the 3/4 reference)',
                           'scaleReference': 'Peter\'s bind mesh: 1.886 units tall (measure.json)'}
spec['silhouette'] = {'boundingShape': 'pieces on a short wide figure: hat on top, vest over the torso, boots at the '
                                       'feet, sheath at the right hip',
                      'aspectRatios': [{'what': 'hat brim width / head width', 'value': 1.2}],
                      'symmetry': 'bilateral except the sheath (right hip only)',
                      'dominantCurves': ['hat brim curl', 'vest belly curve', 'sheath curve'],
                      'negativeSpaces': ['open vest front', 'armholes'],
                      'landmarks': ['brim above the glasses', 'vest hem at the belt', 'boot tops mid-shin']}
spec['assumptions'] = ['The wearer is the game\'s own Peter model (not rebuilt, never shipped); these pieces are '
                       'drawn on top of him and skinned to his skeleton.',
                       'Shirt, jeans and belt colours are a skin recolour of his own texture, not geometry.',
                       'Hidden sides (hat back, vest back, boot heels) inferred by symmetry.']
spec['performanceBudget']['targetTriangles'] = 60000
spec['performanceBudget']['notes'] = 'Game: each vertex is skinned on the CPU every frame; keep the pack small.'
# the pieces are drawn in game by its own toon shader from one palette texture: no PBR map can reach the screen,
# so the look-dev bar is 'balanced' (the materials declare themselves textureless, with crop evidence)
spec['lookDevTargets']['qualityPriority'] = 'balanced'
spec['lookDevTargets']['notes'] = ('Real-time game asset rendered by the game\'s own cel shader with a '
                                   'palette texture; flat albedo per part is the measured reference finish.')
pa = spec['preSpecAssessment']
pa['complexity']['scores']['componentCount'] = 3
spec['assumptions'] += ['Resolved unknown: ' + u for u in pa['unknownsToResolveBeforeImplementation']]
pa['unknownsToResolveBeforeImplementation'] = []
spec['lightingFromPhoto'] = [
    {'id': 'key', 'type': 'directional', 'direction': [-0.4, 0.8, 0.6], 'intensity': 2.2, 'color': '#FFFFFF',
     'notes': 'Reference is flat cel art lit from the upper front-left; review renders use a key from there.'},
    {'id': 'fill', 'type': 'hemisphere', 'skyColor': '#FFFFFF', 'groundColor': '#8A7F78', 'intensity': 1.1,
     'notes': 'Soft fill so shadow sides keep the cel shadow tone instead of going black.'},
    {'id': 'rim', 'type': 'directional', 'direction': [0.5, 0.4, -0.8], 'intensity': 0.6, 'color': '#FFFFFF',
     'notes': 'Rim / environment light separating the silhouette from the white review background.'},
    {'id': 'exposure', 'type': 'tone', 'toneMapping': 'ACES filmic', 'exposure': 1.0,
     'notes': 'Exposure 1.0 with ACES tone mapping in the review harness; in game the toon shader decides.'},
    {'id': 'contact', 'type': 'shadow', 'notes': 'Contact shadow under the boots on a ground plane (review only).'},
]
json.dump(spec, open('object-sculpt-spec.json', 'w', encoding='utf-8'), indent=2, ensure_ascii=False)
print('components', len(C))
