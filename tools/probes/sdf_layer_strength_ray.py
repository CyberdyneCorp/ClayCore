"""What does a strength slider PROMISE, and does a field lerp keep it?

A mesh sculpt layer's strength scales a DISPLACEMENT: at 0.5 the surface sits
half as far from the base as at 1.0. That is what an artist reads the slider as,
and it is exact by construction there.

The obvious SDF analogue is a lerp of the field:

    d_s(p) = (1 - s) * d_base(p) + s * d_full(p)

This measures whether that keeps the promise. Along a probe ray it finds the
surface crossing for each s and reports how far it moved from the base, as a
FRACTION of the full move. A slider that behaves reads 0.50 at s=0.50.
"""
import sys
sys.path.insert(0, 'build/cpu-only/bindings/python')
import numpy as np
import pyclay


def clay_blend(k):
    b = pyclay.Blend(); b.k = k; return b


def surface_t(doc_b, doc_f, s, origin, direction, tmax=1.2, n=4001):
    """First sign change of the lerped field along a ray, to sub-sample accuracy."""
    ts = np.linspace(0.0, tmax, n, dtype=np.float32)
    pts = (np.asarray(origin, dtype=np.float32)[None, :]
           + ts[:, None] * np.asarray(direction, dtype=np.float32)[None, :])
    db = np.asarray(doc_b.eval(pts), dtype=np.float64)
    df = np.asarray(doc_f.eval(pts), dtype=np.float64)
    d = (1.0 - s) * db + s * df
    sign = np.sign(d)
    idx = np.flatnonzero(sign[:-1] * sign[1:] < 0)
    if idx.size == 0:
        return None
    i = idx[0]
    # linear interpolation between the bracketing samples
    a, b = d[i], d[i + 1]
    return float(ts[i] + (ts[i + 1] - ts[i]) * (a / (a - b)))


def case(name, build_base, build_full, origin, direction):
    b, f = pyclay.Document(), pyclay.Document()
    build_base(b); build_full(f)
    t0 = surface_t(b, f, 0.0, origin, direction)
    t1 = surface_t(b, f, 1.0, origin, direction)
    if t0 is None or t1 is None:
        print(f"{name}: no crossing"); return
    span = t1 - t0
    print(f"\n{name}")
    print(f"  surface at s=0: t={t0:.5f}   at s=1: t={t1:.5f}   full move {span:+.5f}")
    print(f"  {'s':>5}  {'t':>9}  {'moved':>9}  {'fraction':>9}  {'linear?':>8}")
    worst = 0.0
    for s in [0.1 * i for i in range(11)]:
        t = surface_t(b, f, s, origin, direction)
        if t is None:
            print(f"  {s:5.2f}  {'--':>9}  {'--':>9}  {'--':>9}   SURFACE GONE")
            continue
        frac = (t - t0) / span if abs(span) > 1e-9 else 0.0
        err = frac - s
        worst = max(worst, abs(err))
        print(f"  {s:5.2f}  {t:9.5f}  {t - t0:+9.5f}  {frac:9.3f}  {err:+8.3f}")
    print(f"  worst departure from a linear slider: {worst:.3f}")
    return worst


# 1. UNION -- a bar added on top of a sphere. The easy case.
def u_base(d):
    l = d.add_sdf_layer("base"); l.add(pyclay.Sphere(r=0.5))
def u_full(d):
    l = d.add_sdf_layer("base"); l.add(pyclay.Sphere(r=0.5))
    m = d.add_sdf_layer("pass"); m.add(pyclay.Box(size=(0.7, 0.7, 0.12), position=(0, 0, 0.5)))

# 2. SUBTRACT -- a channel carved into the sphere. The op a "damage" pass uses.
def s_base(d):
    l = d.add_sdf_layer("base"); l.add(pyclay.Sphere(r=0.5))
def s_full(d):
    l = d.add_sdf_layer("base"); l.add(pyclay.Sphere(r=0.5))
    m = d.add_sdf_layer("pass")
    m.add(pyclay.Sphere(r=0.22, position=(0, 0, 0.52)))
    d.set_layer_composition(m.id, op=pyclay.Op.SUBTRACT)

# 3. SMOOTH UNION -- the same bar, blended. What a "secondary forms" pass looks like.
def b_base(d):
    l = d.add_sdf_layer("base"); l.add(pyclay.Sphere(r=0.5))
def b_full(d):
    l = d.add_sdf_layer("base"); l.add(pyclay.Sphere(r=0.5))
    m = d.add_sdf_layer("pass")
    m.add(pyclay.Sphere(r=0.30, position=(0, 0, 0.45)))
    d.set_layer_composition(m.id, op=pyclay.Op.ADD, blend=pyclay.Smooth(0.25))

# 4. INTERSECT -- a trim. The op that can remove the surface entirely.
def i_base(d):
    l = d.add_sdf_layer("base"); l.add(pyclay.Sphere(r=0.5))
def i_full(d):
    l = d.add_sdf_layer("base"); l.add(pyclay.Sphere(r=0.5))
    m = d.add_sdf_layer("pass")
    m.add(pyclay.Box(size=(0.6, 0.6, 0.35)))
    d.set_layer_composition(m.id, op=pyclay.Op.INTERSECT)

# 5. TWO OPS IN ONE PASS -- add here, carve there. What a real pass looks like,
#    and the case a single scalar has to describe both halves of.
def m_base(d):
    l = d.add_sdf_layer("base"); l.add(pyclay.Sphere(r=0.5))
def m_full(d):
    l = d.add_sdf_layer("base"); l.add(pyclay.Sphere(r=0.5))
    m = d.add_sdf_layer("pass")
    m.add(pyclay.Sphere(r=0.25, position=(0, 0, 0.5)))
    m.add(pyclay.Sphere(r=0.20, position=(0.42, 0, 0.30)), op=pyclay.Op.SUBTRACT)
    # the pass folds as a union; the SUBTRACT above is WITHIN the pass

O, D = (0.0, 0.0, 1.2), (0.0, 0.0, -1.0)
w = []
w.append(case("1. UNION      (a bar laid on the sphere)", u_base, u_full, O, D))
w.append(case("2. SUBTRACT   (a channel carved into it)", s_base, s_full, O, D))
w.append(case("3. SMOOTH ADD (a blended bump)", b_base, b_full, O, D))
w.append(case("4. INTERSECT  (a trim through the probe)", i_base, i_full, O, D))
w.append(case("5. ADD+SUB    (one pass, two ops)", m_base, m_full, O, D))
print()
print("A slider an artist can trust reads fraction == s at every stop.")
print(f"worst departures: {['%.3f' % x if x is not None else '--' for x in w]}")
