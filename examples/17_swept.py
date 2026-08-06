"""Sweeping profiles along a guide.

This is the second half of Track A. `Loft` interpolates profiles along the
item's Z axis; `Swept` carries the same profiles along a **guide curve** — and
the plan's assumption that this was "the same opcode with a count" turned out
to be wrong, because loft already takes N. The real content is the guide.

Three things are worth reading before looking.

**The frame is parallel-transported, not derived.** A Frenet frame — the one
you get from the curve's own derivatives — flips at an inflection point and is
undefined where the curve is straight. A sweep built on one would twist
visibly at exactly the places it should be calmest. Transport is sequential
along the curve, so it cannot be computed per sample; the compiler walks the
guide once and stores a frame per vertex. The third panel below is the test of
that: a guide that bends, straightens, then bends back, with a flat profile
whose orientation would be obvious if it flipped.

**Profiles are distributed by arc length.** A guide whose points bunch does not
bunch the profiles.

**The ends are the profile, not a rounded cap.** A profile need not be a
circle, so there is no hemisphere to close it with. On the axis just past the
end, the distance is the overshoot past a flat face.

And the honest limitation: a sweep compresses space on the inside of a bend by
`R / (R - r)`. A profile wider than the guide's tightest bend folds the sweep
through itself. That is **not refused** — a guide is editable after the fact,
so it has to degrade rather than fail — and the safe step scale collapses so
the raymarcher crawls instead of stepping through a surface it was told was a
distance field. The last section prints that collapse.
"""

import numpy as np

import pyclay as clay

import _render as R


def swept_doc(guide, profiles, colour="#b0784a", **kwargs):
    doc = clay.Document()
    doc.add_sdf_layer("l").add(clay.Swept(guide, profiles, **kwargs), color=colour)
    return doc


def guide_from(points):
    """(N, 4) guide points — the radius column is unused; a sweep's shape is
    in its profiles, not in its guide."""
    return np.array([[p[0], p[1], p[2], 0.0] for p in points], np.float32)


def helix(turns=1.25, radius=0.8, rise=1.6, n=24):
    t = np.linspace(0.0, turns * 2.0 * np.pi, n)
    return guide_from(list(zip(np.cos(t) * radius,
                               np.linspace(-rise / 2, rise / 2, n),
                               np.sin(t) * radius)))


def main():
    R.banner("17 swept — profiles carried along a guide")

    EYE, TARGET = (2.6, 1.9, 2.8), (0, 0, 0)

    # --- what a sweep is -----------------------------------------------------
    cases = [
        ("straight", guide_from([(-1.2, 0, 0), (1.2, 0, 0)]),
         [clay.Profile.circle(r=0.3), clay.Profile.circle(r=0.3)], "hard"),
        ("tapered", guide_from([(-1.2, 0, 0), (1.2, 0, 0)]),
         [clay.Profile.circle(r=0.4), clay.Profile.circle(r=0.08)], "hard"),
        # r = 0.12, not 0.25: this guide's tightest circumradius is 0.239, so
        # a 0.25 tube would fold through itself at the apex — which the last
        # section demonstrates deliberately rather than by accident here.
        ("bent", guide_from([(-1.1, -0.5, 0), (0, 0.6, 0.3), (1.1, -0.5, 0)]),
         [clay.Profile.circle(r=0.12), clay.Profile.circle(r=0.12)], "spline"),
        ("helical", helix(),
         [clay.Profile.circle(r=0.18), clay.Profile.circle(r=0.18)], "spline"),
    ]
    tiles = []
    for name, guide, profiles, types in cases:
        doc = swept_doc(guide, profiles, types=types, tolerance=0.01)
        print(f"  {name:10s} step scale {doc.safe_step_scale():.3f}")
        if doc.safe_step_scale() <= 0.0:
            raise SystemExit(f"the {name} sweep collapsed — its profile has outgrown its guide")
        tiles.append(R.render_array(doc, eye=EYE, target=TARGET, width=205, height=195))
    R.contact_sheet(tiles, "17_swept_guides.png", columns=4,
                    caption=", ".join(name for name, _, _, _ in cases))

    # --- the frame does not flip ---------------------------------------------
    # Bend, straighten, bend back, with a flat profile. A Frenet frame is
    # undefined on the straight middle; a transported one carries through.
    wavy = guide_from([(-2.0, 0.0, 0), (-1.0, 0.4, 0), (0.0, 0.4, 0),
                       (1.0, 0.4, 0), (2.0, 0.0, 0)])
    flat = [clay.Profile.box(half_x=0.30, half_y=0.06),
            clay.Profile.box(half_x=0.30, half_y=0.06)]
    doc = swept_doc(wavy, flat, types="hard")

    # The thin direction must stay put along the straight middle: a flip would
    # swap thin for wide and these signs would change.
    xs = np.arange(-0.8, 0.81, 0.2)
    thin = np.array([[x, 0.4, 0.20] for x in xs], np.float32)
    wide = np.array([[x, 0.4, 0.02] for x in xs], np.float32)
    held = bool((doc.eval(thin) > 0).all() and (doc.eval(wide) < 0).all())
    print(f"  the frame holds its orientation across the straight middle: {held}")
    if not held:
        raise SystemExit("the frame flipped where the guide straightens")
    R.render(doc, "17_swept_frame.png", eye=(0.6, 2.4, 2.6), target=(0, 0.2, 0),
             caption="a flat profile through bend-straight-bend: the frame does not flip")

    # --- arc length, not vertex index ----------------------------------------
    even = guide_from([(-1, 0, 0), (1, 0, 0)])
    bunched = guide_from([(-1, 0, 0), (-0.9, 0, 0), (1, 0, 0)])
    taper = [clay.Profile.circle(r=0.4), clay.Profile.circle(r=0.1)]
    a = swept_doc(even, taper, types="hard")
    b = swept_doc(bunched, taper, types="hard")
    probes = np.array([[x, 0.2, 0] for x in np.arange(-0.9, 0.91, 0.1)], np.float32)
    same = bool(np.allclose(a.eval(probes), b.eval(probes), atol=0.05))
    print(f"  an extra bunched guide point does not shift the taper: {same}")
    if not same:
        raise SystemExit("profiles are being placed per vertex, not per arc length")

    # --- flat ends -----------------------------------------------------------
    tube = swept_doc(guide_from([(-1, 0, 0), (1, 0, 0)]),
                     [clay.Profile.circle(r=0.3), clay.Profile.circle(r=0.3)], types="hard")
    past_end = float(tube.eval(np.array([[-1.4, 0, 0]], np.float32))[0])
    print(f"  0.4 past the end reads {past_end:.3f} — a flat cap, not a rounded one")
    if abs(past_end - 0.4) > 0.02:
        raise SystemExit("the end cap is no longer the profile")

    # --- curvature costs steps, and an overgrown profile degrades ------------
    print("  the safe step scale against the guide's tightest bend:")
    scales = []
    for name, peak in (("gentle", 0.2), ("moderate", 0.7), ("sharp", 1.6)):
        doc = swept_doc(guide_from([(-2, 0, 0), (0, peak, 0), (2, 0, 0)]),
                        [clay.Profile.circle(r=0.2), clay.Profile.circle(r=0.2)], types="hard")
        scales.append(doc.safe_step_scale())
        print(f"    {name:9s} peak {peak:<4} -> {scales[-1]:.4f}")
    if not (scales[0] > scales[1] > scales[2]):
        raise SystemExit("the step scale no longer tracks the guide's curvature")

    folded = swept_doc(guide_from([(-1, 0, 0), (0, 1.0, 0), (1, 0, 0)]),
                       [clay.Profile.circle(r=2.0), clay.Profile.circle(r=2.0)], types="hard")
    value = float(folded.eval(np.array([[0, 0, 0]], np.float32))[0])
    print(f"    a profile wider than the bend folds through itself: step "
          f"{folded.safe_step_scale():.5f}, still evaluates ({value:.3f})")
    if not (folded.safe_step_scale() < 0.01 and np.isfinite(value)):
        raise SystemExit("an overgrown profile no longer degrades honestly")

    # --- sweeps are ordinary items -------------------------------------------
    doc = clay.Document()
    layer = doc.add_sdf_layer("l")
    layer.add(clay.Sphere(r=1.0), color="#7f8a94")
    layer.add(clay.Swept(helix(turns=1.6, radius=1.05, rise=2.4),
                         [clay.Profile.circle(r=0.12), clay.Profile.circle(r=0.12)],
                         types="spline", tolerance=0.01),
              op=clay.Op.SUBTRACT)
    R.render(doc, "17_swept_carved.png", eye=(2.6, 1.6, 2.6), target=TARGET,
             caption="a helical sweep subtracted from a sphere")

    R.export_model(swept_doc(helix(), [clay.Profile.circle(r=0.18),
                                       clay.Profile.circle(r=0.18)],
                             types="spline", tolerance=0.01),
                   "17_swept.ply", resolution=64, decimate=0.1)


if __name__ == "__main__":
    main()
