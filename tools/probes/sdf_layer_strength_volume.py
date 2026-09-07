"""The same question, with an instrument that can answer no.

strength_probe2 measured the surface crossing along a ray and reported EXACTLY
linear for all five ops. That is not a result, it is the probe's shape: the ray
was normal-incident, both fields have unit gradient along it, so the lerped
crossing moves linearly by construction. A probe that cannot report non-linear
has not checked anything.

So: occupied VOLUME over the whole grid, which is global and cannot be dodged by
a lucky ray -- plus a control case built to be non-linear, so the instrument is
shown capable of saying no before its zeros are believed.
"""
import sys
sys.path.insert(0, 'build/cpu-only/bindings/python')
import numpy as np
import pyclay


def volumes(build_base, build_full, n=72, half=1.3):
    b, f = pyclay.Document(), pyclay.Document()
    build_base(b); build_full(f)
    ax = np.linspace(-half, half, n, dtype=np.float32)
    X, Y, Z = np.meshgrid(ax, ax, ax, indexing="ij")
    pts = np.stack([X.ravel(), Y.ravel(), Z.ravel()], axis=1).astype(np.float32)
    db = np.asarray(b.eval(pts), dtype=np.float64)
    df = np.asarray(f.eval(pts), dtype=np.float64)
    cell = (2.0 * half / (n - 1)) ** 3
    out = []
    for i in range(11):
        s = i / 10.0
        d = (1.0 - s) * db + s * df
        out.append((s, float((d < 0).sum()) * cell))
    return out


def report(name, rows):
    v0, v1 = rows[0][1], rows[-1][1]
    span = v1 - v0
    print(f"\n{name}")
    print(f"  volume s=0 {v0:.5f}  s=1 {v1:.5f}  span {span:+.5f}")
    worst = 0.0
    for s, v in rows:
        frac = (v - v0) / span if abs(span) > 1e-12 else 0.0
        err = frac - s
        worst = max(worst, abs(err))
        print(f"  s={s:4.2f}  vol {v:9.5f}  fraction {frac:7.3f}  err {err:+7.3f}")
    print(f"  worst departure from linear: {worst:.3f}")
    return worst


# CONTROL: the instrument must be able to say NO. A layer whose contribution is
# a THIN SHELL far from the base surface -- the lerped field crosses zero over a
# region whose volume grows as a step, not a ramp.
def c_base(d):
    l = d.add_sdf_layer("base"); l.add(pyclay.Sphere(r=0.5))
def c_full(d):
    l = d.add_sdf_layer("base"); l.add(pyclay.Sphere(r=0.5))
    m = d.add_sdf_layer("pass")
    # a big shell that only becomes occupied once s pulls it under zero
    m.add(pyclay.Sphere(r=1.15))
    d.set_layer_composition(m.id, op=pyclay.Op.ADD)

def u_base(d):
    l = d.add_sdf_layer("base"); l.add(pyclay.Sphere(r=0.5))
def u_full(d):
    l = d.add_sdf_layer("base"); l.add(pyclay.Sphere(r=0.5))
    m = d.add_sdf_layer("pass"); m.add(pyclay.Box(size=(0.7, 0.7, 0.12), position=(0, 0, 0.5)))

def s_base(d):
    l = d.add_sdf_layer("base"); l.add(pyclay.Sphere(r=0.5))
def s_full(d):
    l = d.add_sdf_layer("base"); l.add(pyclay.Sphere(r=0.5))
    m = d.add_sdf_layer("pass"); m.add(pyclay.Sphere(r=0.22, position=(0, 0, 0.52)))
    d.set_layer_composition(m.id, op=pyclay.Op.SUBTRACT)

def b_base(d):
    l = d.add_sdf_layer("base"); l.add(pyclay.Sphere(r=0.5))
def b_full(d):
    l = d.add_sdf_layer("base"); l.add(pyclay.Sphere(r=0.5))
    m = d.add_sdf_layer("pass"); m.add(pyclay.Sphere(r=0.30, position=(0, 0, 0.45)))
    d.set_layer_composition(m.id, op=pyclay.Op.ADD, blend=pyclay.Smooth(0.25))

w = {}
w["CONTROL (a shell that appears)"] = report("CONTROL (a shell that appears)", volumes(c_base, c_full))
w["UNION"] = report("UNION (a bar on the sphere)", volumes(u_base, u_full))
w["SUBTRACT"] = report("SUBTRACT (a channel carved)", volumes(s_base, s_full))
w["SMOOTH"] = report("SMOOTH ADD (a blended bump)", volumes(b_base, b_full))
print()
for k, v in w.items():
    print(f"  {k:32s} worst {v:.3f}")
print()
print("If the CONTROL reads ~0 the instrument is broken and the others mean nothing.")
