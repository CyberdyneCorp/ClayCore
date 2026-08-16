"""Deformers: twist, bend, taper and displace, including chained order.

Deformers warp the point before the primitive is evaluated, so they are
metric breakers — the field stops being a true distance function and the
library tracks a Lipschitz bound instead. Each tile prints the resulting safe
step scale, which is the number sphere tracing has to slow down by.
"""

import pathlib

import numpy as np

import pyclay as clay

import _render as R


def _s_curve():
    """An S: it turns one way and then the other, which no constant-rate bend
    can do. Handed over as control points and smoothed by the default B-spline,
    the same way any other curve in a document is authored."""
    x = np.linspace(-1.1, 1.1, 5)
    return np.stack([x, 0.42 * np.sin(np.pi * x / 1.1), np.zeros_like(x)],
                    axis=1).astype(np.float32)


def deformed(make):
    doc = clay.Document()
    layer = doc.add_sdf_layer("l")
    layer.add(make())
    return doc, layer


CASES = [
    ("undeformed", lambda: clay.Box(size=(0.7, 1.8, 0.7))),
    ("twist 1.5", lambda: clay.Box(size=(0.7, 1.8, 0.7)).twist(1.5)),
    ("twist 3.0", lambda: clay.Box(size=(0.7, 1.8, 0.7)).twist(3.0)),
    ("bend 0.8", lambda: clay.Box(size=(0.7, 1.8, 0.7)).bend(0.8)),
    # The RANGED pair: the same rotations, ramped across a span and HELD
    # beyond it — which is what a gizmo's box does and what `twist` above
    # cannot express. The material past the span travels rigidly instead of
    # continuing to wind, so the top of this box is straight where "twist 3.0"
    # is still turning.
    ("twist_range", lambda: clay.Box(size=(0.7, 1.8, 0.7)).twist_range(
        radians_per_unit=3.0, y0=-0.5, y1=0.5, ease=3)),
    ("bend_range", lambda: clay.Box(size=(0.7, 1.8, 0.7)).bend_range(
        radians_per_unit=1.4, x0=-0.35, x1=0.35, ease=3)),
    # bend_curve bends along a DRAWN guide, so the axis takes a shape no
    # constant rate can produce — every bend `bend` can express is a circular
    # arc, and this one changes its mind halfway. The inverse of a sweep: the
    # box's X span is laid onto the guide's arc length and the material rides
    # the guide's frames.
    ("bend_curve", lambda: clay.Box(size=(1.8, 0.32, 0.32)).bend_curve(
        guide=_s_curve(), t0=-0.9, t1=0.9)),
    ("taper", lambda: clay.Box(size=(0.9, 1.8, 0.9)).taper(
        y0=-0.9, y1=0.9, s0=1.0, s1=0.25)),
    ("displace", lambda: clay.Sphere(r=0.9).displace(amplitude=0.08, frequency=7.0)),
    # wrap_around bends a flat slab around a cylinder — the deformer that
    # cannot be faked with the others. The interval fixes the radius.
    ("wrap_around", lambda: clay.Box(size=(6.283, 0.35, 1.2)).wrap_around(-3.1416, 3.1416)),
    # elongate inserts flat sections: a sphere becomes a capsule, and unlike
    # every other deformer it is exact, so it costs nothing in step scale.
    ("elongate", lambda: clay.Sphere(r=0.45).elongate((0.8, 0.0, 0.0))),
    # the ramped bends: displacement eased across a span, not a rotation
    ("bend_linear", lambda: clay.Box(size=(0.5, 1.8, 0.5)).bend_linear(
        a=(0, -0.9, 0), b=(0, 0.9, 0), v=(0.9, 0, 0), ease=3)),
    ("bend_radial", lambda: clay.Cylinder(r=1.0, h=0.12).bend_radial(
        r0=0.1, r1=1.0, dz=0.7, ease=5)),
    # elongate_axis stretches an asymmetric primitive, which elongate cannot
    ("elongate_axis", lambda: clay.Cone(h=0.6, r1=0.5, r2=0.1).elongate_axis((0.7, 0, 0))),
    # The region deformers are the only ones with finite support: outside the
    # radius the field is untouched, which is what makes them behave like a
    # sculpting brush rather than a whole-item modifier.
    # blob is ZBrush's: noise with the SAME finite support grab and magnify
    # have, so it is a brush rather than a modifier. One dab both swells and
    # eats in, because the amplitude is signed and so is the noise.
    ("blob", lambda: clay.Sphere(r=0.8).blob(
        center=(0.55, 0.45, 0.35), radius=0.75, amplitude=0.11, frequency=9.0,
        octaves=4, seed=5, ease=3)),
    ("grab", lambda: clay.Sphere(r=0.8).grab(
        center=(0.8, 0, 0), radius=0.7, displacement=(0.5, 0.25, 0), ease=3)),
    # pose_line ramps the rotation along a segment, which is how a limb tapers
    ("pose_line", lambda: clay.Capsule(a=(0, -0.9, 0), b=(0, 0.9, 0), r=0.22).pose_line(
        a=(0, -0.9, 0), b=(0, 0.9, 0), axis=(0, 0, 1), angle=1.1, ease=3)),
    ("pose", lambda: clay.Cylinder(r=0.25, h=0.9).pose(
        center=(0, 0.7, 0), radius=0.9, axis=(0, 0, 1), angle=1.0, ease=2)),
    # Chains apply in authoring order — twist then bend is not bend then twist.
    ("twist then bend", lambda: clay.Box(size=(0.7, 1.8, 0.7)).twist(2.0).bend(0.6)),
    ("bend then twist", lambda: clay.Box(size=(0.7, 1.8, 0.7)).bend(0.6).twist(2.0)),
]


# The shape each case deforms, so the check below asks "did the warp move this
# primitive?" rather than comparing unlike tiles. `at`, `repeat_grid` and
# `repeat_radial` are placement rather than domain warps and are covered by
# 04_repetition.py; `noise` is covered by 24_noise.py, where a hash-based field
# needs its own page.
BASE_OF = {
    "twist 1.5":       lambda: clay.Box(size=(0.7, 1.8, 0.7)),
    "twist 3.0":       lambda: clay.Box(size=(0.7, 1.8, 0.7)),
    "bend 0.8":        lambda: clay.Box(size=(0.7, 1.8, 0.7)),
    "taper":           lambda: clay.Box(size=(0.9, 1.8, 0.9)),
    "displace":        lambda: clay.Sphere(r=0.9),
    "wrap_around":     lambda: clay.Box(size=(6.283, 0.35, 1.2)),
    "elongate":        lambda: clay.Sphere(r=0.45),
    "bend_linear":     lambda: clay.Box(size=(0.5, 1.8, 0.5)),
    "bend_radial":     lambda: clay.Cylinder(r=1.0, h=0.12),
    "elongate_axis":   lambda: clay.Cone(h=0.6, r1=0.5, r2=0.1),
    "grab":            lambda: clay.Sphere(r=0.8),
    "pose_line":       lambda: clay.Capsule(a=(0, -0.9, 0), b=(0, 0.9, 0), r=0.22),
    "pose":            lambda: clay.Cylinder(r=0.25, h=0.9),
    "twist then bend": lambda: clay.Box(size=(0.7, 1.8, 0.7)),
    "bend then twist": lambda: clay.Box(size=(0.7, 1.8, 0.7)),
}

COVERED_ELSEWHERE = {
    "magnify":       ("23_magnify.py", "pinch and magnify are one signed strength"),
    "noise":         ("24_noise.py", "a hash-based field earns its own page"),
    "at":            ("11_masks.py", "placement, not a domain warp — it sets a position"),
    "repeat_grid":   ("04_repetition.py", "placement, not a domain warp"),
    "repeat_radial": ("04_repetition.py", "placement, not a domain warp"),
    "gate":          ("54_masked_operations.py",
                      "not a domain warp at all — it gates whether the item "
                      "PARTICIPATES, and the page it earns is about protecting "
                      "a surface from a boolean"),
    "lattice":       ("50_sdf_lattice.py",
                      "a cage is not a one-line warp, and the page it earns is "
                      "about what running FFD backwards costs"),
    "alpha":         ("53_sdf_alphas.py",
                      "a stamp needs an image to be about, and the page it earns "
                      "is about what its steepness costs in step scale"),
}


def deformer_methods():
    """Every warp a prim exposes, taken from the binding rather than a list
    here, so a new one shows up as a gap instead of being forgotten."""
    p = clay.Sphere(r=1.0)
    return {m for m in dir(p) if not m.startswith("_") and callable(getattr(p, m))}


def main():
    R.banner("03 deformers — domain warps and what they cost")

    tiles = []
    for name, make in CASES:
        doc, layer = deformed(make)
        step = doc.safe_step_scale()
        print(f"  {name:18s} safe step scale {step:.3f}")
        tiles.append(R.render_tile(doc, layer=layer, size=160))
    R.contact_sheet(tiles, "03_deformers.png", columns=4,
                    caption=", ".join(n for n, _ in CASES))

    # A displaced sphere makes a decent rock; mesh and export one.
    doc = clay.Document()
    layer = doc.add_sdf_layer("l")
    layer.add(clay.Sphere(r=1.0).displace(amplitude=0.12, frequency=5.0),
              color="#8a7f6d")
    eye, target = R.layer_camera(layer)
    R.render(doc, "03_displaced.png", eye=eye, target=target,
             colors_from_field=True, caption="displacement as surface detail")
    R.export_model(doc, "03_displaced.ply", resolution=64)

    # --- what the pictures cannot check ---------------------------------------
    # A deformer that quietly stopped warping renders as the undeformed tile,
    # and an undeformed tile is a perfectly good-looking picture. So each case
    # has to MOVE the field relative to the shape it deformed.
    rng = np.random.default_rng(3)
    probe = rng.uniform(-1.6, 1.6, size=(4000, 3)).astype(np.float32)

    def field(make):
        d = clay.Document()
        d.add_sdf_layer("l").add(make())
        return d.eval(probe)

    inert = []
    for name, make in CASES:
        if name == "undeformed":
            continue
        base = BASE_OF.get(name)
        if base is None:
            continue
        if float(np.abs(field(make) - field(base)).max()) < 1e-4:
            inert.append(name)
    if inert:
        raise SystemExit(f"these deformers changed nothing: {inert}")
    print(f"  all {len(BASE_OF)} deformer cases move the field they warp")

    # Chain ORDER is part of the edit: twist-then-bend is not bend-then-twist.
    # The two tiles look similar enough that only a measurement separates them.
    box = lambda: clay.Box(size=(0.7, 1.8, 0.7))
    tb = field(lambda: box().twist(2.0).bend(0.6))
    bt = field(lambda: box().bend(0.6).twist(2.0))
    order = float(np.abs(tb - bt).max())
    if order < 1e-3:
        raise SystemExit(f"deformer order stopped mattering (max delta {order:.2e})")
    print(f"  chain order matters: twist->bend vs bend->twist differ by {order:.3f}")

    # elongate is the one deformer that is EXACT, so unlike every other it must
    # not cost step scale. If it ever starts to, the claim in the table above is
    # wrong rather than merely optimistic.
    plain = clay.Document(); plain.add_sdf_layer("l").add(clay.Sphere(r=0.45))
    elong = clay.Document()
    elong.add_sdf_layer("l").add(clay.Sphere(r=0.45).elongate((0.8, 0.0, 0.0)))
    if elong.safe_step_scale() < plain.safe_step_scale() - 1e-6:
        raise SystemExit("elongate cost step scale — it is supposed to stay exact")
    print(f"  elongate stayed exact: step scale {elong.safe_step_scale():.3f}")

    # The region deformers have FINITE support: past the radius the field is
    # untouched. That is what makes them brushes rather than item modifiers,
    # and it is invisible in a tile cropped to the deformed part.
    far = np.array([[3.0, 3.0, 3.0], [-2.5, 0.0, 2.0]], dtype=np.float32)
    for name, make in (("grab", lambda: clay.Sphere(r=0.8).grab(
                            center=(0.8, 0, 0), radius=0.7,
                            displacement=(0.5, 0.25, 0), ease=3)),
                       ("pose", lambda: clay.Sphere(r=0.8).pose(
                            center=(0.8, 0, 0), radius=0.7,
                            axis=(0, 0, 1), angle=1.0, ease=2)),
                       ("magnify", lambda: clay.Sphere(r=0.8).magnify(
                            center=(0.8, 0, 0), radius=0.7, strength=0.5, ease=2)),
                       ("blob", lambda: clay.Sphere(r=0.8).blob(
                            center=(0.8, 0, 0), radius=0.7, amplitude=0.15,
                            frequency=9.0, octaves=4, seed=5, ease=2))):
        d = clay.Document(); d.add_sdf_layer("l").add(make())
        p = clay.Document(); p.add_sdf_layer("l").add(clay.Sphere(r=0.8))
        outside = float(np.abs(d.eval(far) - p.eval(far)).max())
        if outside > 1e-5:
            raise SystemExit(f"{name} reached {outside:.2e} outside its radius — support is not finite")
    print("  grab, pose, magnify and blob leave the field untouched past their radius")

    # Coverage: a deformer with no case here is a gap in the gallery. Read
    # from this file's own source, so a case added to CASES counts and a method
    # that never appears does not.
    src = pathlib.Path(__file__).read_text()
    cases_src = src[src.index("CASES = ["):src.index("BASE_OF = {")]
    shown = {m for m in deformer_methods() if f".{m}(" in cases_src}
    missing = deformer_methods() - shown - set(COVERED_ELSEWHERE)
    if missing:
        raise SystemExit(f"deformers with no example: {sorted(missing)}")
    for name, (where, why) in sorted(COVERED_ELSEWHERE.items()):
        if name in shown:
            raise SystemExit(f"{name} has a tile here now — drop it from COVERED_ELSEWHERE")
        # Check the claim rather than taking it: an exemption pointing at a file
        # that stopped using the deformer is how coverage rots quietly.
        elsewhere = pathlib.Path(__file__).with_name(where)
        if not elsewhere.exists() or f".{name}(" not in elsewhere.read_text():
            raise SystemExit(f"{name} is claimed to be covered by {where}, which does not use it")
        print(f"  {name} is covered by {where}: {why}")
    print(f"  covered all {len(deformer_methods())} deformer methods")


if __name__ == "__main__":
    main()
