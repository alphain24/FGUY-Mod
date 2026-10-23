"""Evaluates an img2threejs geometryDescriptor.sdf in Python exactly as the generated factory does
(sdfPrimitive / smin / subtract / intersect), so parts that sit on an implicit surface (eyelets, laces, pockets,
buttons) are placed on the surface the factory will actually build, fillets included."""
import math


def _euler_inv_apply(rot, p):
    """Apply the inverse of THREE's Euler XYZ rotation (R = Rx Ry Rz) to p: R^T p."""
    cx, sx, cy, sy, cz, sz = (math.cos(rot[0]), math.sin(rot[0]), math.cos(rot[1]), math.sin(rot[1]),
                              math.cos(rot[2]), math.sin(rot[2]))
    rx = [[1, 0, 0], [0, cx, -sx], [0, sx, cx]]
    ry = [[cy, 0, sy], [0, 1, 0], [-sy, 0, cy]]
    rz = [[cz, -sz, 0], [sz, cz, 0], [0, 0, 1]]
    mul = lambda a, b: [[sum(a[i][k] * b[k][j] for k in range(3)) for j in range(3)] for i in range(3)]
    m = mul(mul(rx, ry), rz)
    return [sum(m[k][i] * p[k] for k in range(3)) for i in range(3)]


def _prim(p, q):
    t = q.get('transform') or {}
    c = t.get('position') or t.get('translation') or q.get('center') or [0, 0, 0]
    loc = _euler_inv_apply(t.get('rotation') or [0, 0, 0], [p[i] - c[i] for i in range(3)])
    sc = t.get('scale') or [1, 1, 1]
    loc = [loc[i] / sc[i] for i in range(3)]
    typ = q['type']
    if typ == 'sphere':
        d = math.sqrt(sum(v * v for v in loc)) - q.get('radius', 0.5)
    elif typ == 'capsule':
        h = q.get('height', 1) * 0.5
        y = max(-h, min(h, loc[1]))
        d = math.sqrt(loc[0] ** 2 + (loc[1] - y) ** 2 + loc[2] ** 2) - q.get('radius', 0.25)
    elif typ == 'box':
        size = q.get('size') or q.get('dimensions') or [1, 1, 1]
        qd = [abs(loc[i]) - size[i] / 2 for i in range(3)]
        d = math.sqrt(sum(max(v, 0) ** 2 for v in qd)) + min(max(qd), 0)
    elif typ == 'ellipsoid':
        r = q.get('radii') or (q['radius'] if isinstance(q.get('radius'), list) else [q.get('radius', 0.5)] * 3)
        d = (math.sqrt(sum((loc[i] / r[i]) ** 2 for i in range(3))) - 1) * min(r)
    elif typ == 'cone':
        h, rad = q.get('height', 1), q.get('radius', 0.5)
        taper = rad * (1 - (loc[1] + h / 2) / h)
        d = max(math.hypot(loc[0], loc[2]) - max(0, taper), abs(loc[1]) - h / 2)
    return d * min(sc)


def _smin(a, b, r):
    k = max(r - abs(a - b), 0) / r
    return min(a, b) - k * k * r * 0.25


def field(sdf, until=None):
    """A function p -> distance for the descriptor; `until` stops at that operation / primitive id."""
    def f(p):
        vals = {q['id']: None for q in sdf['primitives']}
        prims = {q['id']: q for q in sdf['primitives']}

        def get(i):
            if i in prims:
                if vals[i] is None:
                    vals[i] = _prim(p, prims[i])
                return vals[i]
            return vals[i]
        result = get(sdf['primitives'][0]['id'])
        if until in prims:
            return get(until)
        for op in sdf.get('operations', []):
            a, b = get(op['left']), get(op['right'])
            if op['type'] == 'smooth-union':
                v = _smin(a, b, op.get('radius', 0.1))
            elif op['type'] == 'subtract':
                v = max(a, -b)
            else:
                v = max(a, b)
            vals[op.get('id') or op.get('output')] = v
            result = v
            if until and (op.get('id') or op.get('output')) == until:
                return v
        return result
    return f


def surface(f, start, direction, reach=1.0, step=0.004):
    """First point from `start` (inside) along `direction` where the field crosses zero, refined by bisection."""
    prev_t, prev_v = 0.0, f(start)
    t = step
    while t <= reach:
        p = [start[i] + direction[i] * t for i in range(3)]
        v = f(p)
        if prev_v < 0 <= v:
            lo, hi = prev_t, t
            for _ in range(30):
                mid = (lo + hi) / 2
                if f([start[i] + direction[i] * mid for i in range(3)]) < 0:
                    lo = mid
                else:
                    hi = mid
            return [round(start[i] + direction[i] * hi, 5) for i in range(3)]
        prev_t, prev_v = t, v
        t += step
    return None


def normal(f, p, e=0.002):
    g = [(f([p[j] + (e if j == i else 0) for j in range(3)]) - f([p[j] - (e if j == i else 0) for j in range(3)]))
         for i in range(3)]
    ln = math.sqrt(sum(v * v for v in g)) or 1.0
    return [v / ln for v in g]
