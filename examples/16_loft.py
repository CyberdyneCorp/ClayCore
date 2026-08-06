"""Loft: two or more profiles interpolated along the lift axis.

`lift.h` has had a loft since the beginning and no document could use one. The
spec said why in a sentence — *"Loft remains header-only until an item can
carry two profiles"* — and that was the only kernel capability this engine had
that was unreachable from a document.

Two things here are worth reading, not just looking at.

**More than two profiles are bracketed, not averaged.** A wide-narrow-wide list
gives a waist, because the middle profile is actually reached. That is why this
row took N from the start rather than exactly two: nothing about the opcode
wanted to be limited to two, and taking N now means carrying profiles along a
guide later changes where they are *placed* rather than how they are *stored*.

**A loft is a bound, and its Lipschitz is not one.** Interpolating two distance
fields does not give a distance field, and the interpolation adds a term
proportional to how far apart the profiles are over how short a depth they are
mixed across. If the engine reported Lipschitz 1 here, the raymarcher would
step as though the field were a distance and walk straight through the surface.
The example prints the safe step scale falling as the depth shrinks — that
number is the raymarcher being told to be careful.
"""

import numpy as np

import pyclay as clay

import _render as R


def loft_doc(profiles, half_depth=1.0, ease=0, colour="#c08a4a"):
    doc = clay.Document()
    doc.add_sdf_layer("l").add(clay.Loft(profiles, half_depth=half_depth, ease=ease),
                               color=colour)
    return doc


def square(half=0.5):
    return clay.Profile.polygon([(-half, -half), (half, -half), (half, half), (-half, half)])


def star(points=5, outer=0.8, inner=0.35):
    verts = []
    for i in range(points * 2):
        r = outer if i % 2 == 0 else inner
        a = i * np.pi / points
        verts.append((r * np.cos(a), r * np.sin(a)))
    return clay.Profile.polygon(verts)


def main():
    R.banner("16 loft — profiles interpolated along the axis")

    EYE, TARGET = (2.4, 1.7, 2.6), (0, 0, 0)

    # --- what a loft is ------------------------------------------------------
    cases = [
        ("circle to circle", [clay.Profile.circle(r=1.0), clay.Profile.circle(r=0.25)]),
        ("circle to square", [clay.Profile.circle(r=0.9), square(0.55)]),
        ("square to star", [square(0.7), star()]),
        ("waisted (3)", [clay.Profile.circle(r=0.9), clay.Profile.circle(r=0.2),
                         clay.Profile.circle(r=0.9)]),
    ]
    tiles = []
    for name, profiles in cases:
        doc = loft_doc(profiles)
        print(f"  {name:18s} step scale {doc.safe_step_scale():.3f}")
        tiles.append(R.render_array(doc, eye=EYE, target=TARGET, width=205, height=195))
    R.contact_sheet(tiles, "16_loft_profiles.png", columns=4,
                    caption=", ".join(name for name, _ in cases))

    # --- three profiles are bracketed, not averaged --------------------------
    waisted = loft_doc([clay.Profile.circle(r=0.9), clay.Profile.circle(r=0.2),
                        clay.Profile.circle(r=0.9)])
    straight = loft_doc([clay.Profile.circle(r=0.9), clay.Profile.circle(r=0.9)])
    probe = np.array([[0.5, 0, 0.0]], np.float32)
    pinched = bool(waisted.eval(probe)[0] > 0 and straight.eval(probe)[0] < 0)
    print(f"  the middle profile is reached, not interpolated past: {pinched}")
    if not pinched:
        raise SystemExit("three profiles behaved like two — the bracketing is wrong")

    # --- the easing curve shapes the interpolation ---------------------------
    tiles = []
    for name, ease in (("linear", 0), ("smoothstep", 3), ("ease-in", 1)):
        doc = loft_doc([clay.Profile.circle(r=1.0), clay.Profile.circle(r=0.2)], ease=ease)
        tiles.append(R.render_array(doc, eye=EYE, target=TARGET, width=215, height=200))
        print(f"  ease {name:11s} -> step scale {doc.safe_step_scale():.3f}")
    R.contact_sheet(tiles, "16_loft_ease.png", columns=3,
                    caption="the same taper under linear, smoothstep and ease-in")

    # --- a loft is a bound, and says so --------------------------------------
    sphere = clay.Document()
    sphere.add_sdf_layer("l").add(clay.Sphere(r=1.0))
    exact = sphere.safe_step_scale()

    scales = []
    for half_depth in (2.0, 1.0, 0.5, 0.2):
        doc = loft_doc([clay.Profile.circle(r=1.0), clay.Profile.circle(r=0.1)],
                       half_depth=half_depth)
        scales.append(doc.safe_step_scale())
        print(f"  half-depth {half_depth:<4} -> safe step scale {scales[-1]:.3f}")
    print(f"  an exact primitive alone steps at {exact:.3f}")

    # Shallower means the field changes faster along the axis, so the step must
    # fall. If this ever stops holding, the raymarcher is being lied to.
    if not (exact > scales[0] > scales[1] > scales[2] > scales[3]):
        raise SystemExit("the safe step scale no longer tracks the loft's steepness")

    # --- lofts are ordinary items --------------------------------------------
    # Subtracted, blended, deformed: nothing about a loft is special once it is
    # placed, which is the whole reason it went in as a primitive.
    doc = clay.Document()
    layer = doc.add_sdf_layer("l")
    layer.add(clay.Box(size=(1.6, 1.6, 1.6)), color="#7f8a94")
    layer.add(clay.Loft([clay.Profile.circle(r=0.75), square(0.3)], half_depth=1.2),
              op=clay.Op.SUBTRACT)
    inside = float(doc.eval(np.array([[0, 0, 0]], np.float32))[0])
    print(f"  a loft subtracted from a box hollows it: {inside > 0}")
    if inside <= 0:
        raise SystemExit("the loft did not subtract")
    R.render(doc, "16_loft_subtracted.png", eye=(2.2, 1.9, 2.4), target=TARGET,
             caption="a circle-to-square loft subtracted from a block")

    R.export_model(loft_doc([square(0.7), star()]), "16_loft.ply", resolution=64)


if __name__ == "__main__":
    main()
