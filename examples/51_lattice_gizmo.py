"""One cage over a whole layer — the gizmo, not a per-item modifier.

`50_sdf_lattice` gives an item a cage in its *own* local space. That is not how
a gizmo works: ZBrush's acts on the whole subtool. Putting a cage over a
blocked-out form with the per-item deformer means authoring one cage per item,
each in a different frame, and keeping them in step by hand.

`Layer.lattice_gizmo` is the step between — the same step `move_surface` is for
a drag. One world-placed cage in, one lattice deformer per item out, each
expressed in that item's own frame.

**Why this needed a kernel change, and could not just resample.** An item's
frame can be *rotated*, and a lattice box is axis-aligned by construction. So a
world-axis-aligned cage is simply not axis-aligned in a rotated item's local
space, and no per-item box reproduces it. Sampling the world cage onto a
per-item grid would approximate it; instead the deformer carries the placement,

    p' = Tinv( T(p) + D(T(p)) )

and is exact for any transform — `math::Transform` being rigid with uniform
scale, which is the same property `move_surface` leans on to keep a spherical
falloff spherical.

Two things fall out of that, and this script checks both:

**The bound does not change.** With `T = sR` the warp's Jacobian in the item's
frame is `R⁻¹ J R` — similar to the cage-space one, so the same norm. A rotation
and a uniform scale cost no step scale at all.

**Every item is reached.** Unlike a drag, a lattice's displacement outside its
box is *clamped* rather than zero, so material out there travels rigidly with
the nearest part of the cage. Skipping "distant" items would tear the form,
which is why this resolver has no reachability test where `move_surface` has
one.
"""

import numpy as np

import pyclay as clay

import _render as R

BOX = ((-1.0, -1.0, -1.0), (1.0, 1.0, 1.0))
N = 3


def cage(**dragged):
    """An (N^3, 3) drag array. `dragged` maps "i_j_k" to a vector."""
    offsets = np.zeros((N * N * N, 3), dtype=np.float32)
    for key, v in dragged.items():
        i, j, k = (int(c) for c in key.split("_"))
        offsets[(k * N + j) * N + i] = v
    return offsets


def form():
    """A blocked-out figure: several items, one of them ROTATED, so the cage has
    to handle a frame that no axis-aligned per-item box could express.

    Returns the item ids too, since a layer reports its children by id."""
    doc = clay.Document()
    layer = doc.add_sdf_layer("body")
    ids = []
    ids.append(layer.add(clay.RoundBox(size=(0.9, 0.5, 0.5), r=0.10, position=(0, -0.35, 0)),
              color="#8d99ae"))
    ids.append(layer.add(clay.Sphere(r=0.30, position=(0, 0.25, 0)),
                         blend=clay.Smooth(0.12), color="#8d99ae"))
    # A rotated limb. This is the item the transform exists for.
    ids.append(layer.add(clay.Capsule(a=(0, 0, 0), b=(0.55, 0.30, 0), r=0.11,
                                      position=(0.35, 0.05, 0),
                                      rotation_axis_angle=((0, 0, 1), 0.6)),
                         blend=clay.Smooth(0.08), color="#ef476f"))
    ids.append(layer.add(clay.Capsule(a=(0, 0, 0), b=(-0.55, 0.30, 0), r=0.11,
                                      position=(-0.35, 0.05, 0),
                                      rotation_axis_angle=((0, 0, 1), -0.6)),
                         blend=clay.Smooth(0.08), color="#ef476f"))
    return doc, layer, ids


def main():
    R.banner("51 lattice gizmo — one cage over a whole layer")

    doc, layer, ids = form()
    rng = np.random.default_rng(51)
    pts = rng.uniform(-1.5, 1.5, size=(512, 3)).astype(np.float32)
    before = doc.eval(pts)
    print(f"  a blocked-out form: {len(ids)} items, one of them rotated")

    # --- what a cage reaches -------------------------------------------------
    # Every item, and that is deliberate. A lattice's displacement outside its
    # box is CLAMPED rather than zero, so an item out there travels rigidly —
    # skipping it would tear the form.
    reach = layer.lattice_gizmo(box=BOX, offsets=cage(**{"2_1_1": (0.4, 0, 0)}),
                                preview=True)
    print(f"  a cage over it warps {len(reach)} of {len(ids)} items — every one,")
    print(f"    because outside the box the displacement is clamped, not zero")
    if len(reach) != len(ids):
        raise SystemExit("a gizmo cage must reach every item in the layer")

    # ...and an untouched cage does nothing at all.
    if layer.lattice_gizmo(box=BOX, preview=True):
        raise SystemExit("an untouched cage must resolve to nothing")
    print("  an untouched cage resolves to nothing — no no-op deformers")

    # --- the placement is what makes it a gizmo ------------------------------
    print("\n  the same drag, with the cage placed in three different spots:")
    tiles = [R.render_array(doc, eye=(2.0, 1.3, 2.4), target=(0, 0, 0),
                            width=250, height=250, colors_from_field=True)]
    names = ["untouched"]
    for label, position in (("over the left", (-0.55, 0.1, 0.0)),
                            ("over the middle", (0.0, 0.1, 0.0)),
                            ("over the right", (0.55, 0.1, 0.0))):
        d, l, _ = form()
        l.lattice_gizmo(position=position, box=((-0.5, -0.5, -0.5), (0.5, 0.5, 0.5)),
                        offsets=cage(**{"1_2_1": (0.0, 0.55, 0.0)}))
        print(f"    {label:<16} step scale {d.safe_step_scale():.4f}")
        tiles.append(R.render_array(d, eye=(2.0, 1.3, 2.4), target=(0, 0, 0),
                                    width=250, height=250, colors_from_field=True))
        names.append(label)
    R.contact_sheet(tiles, "51_lattice_gizmo.png", columns=4,
                    caption=", ".join(names))

    # --- a rotation and a uniform scale cost no step scale -------------------
    # T = sR, so the warp's Jacobian in the item's frame is R-inverse J R —
    # similar to the cage-space one, hence the same norm.
    print("\n  the same cage, placed three ways:")
    scales = []
    for label, kwargs in (("no placement", {}),
                          ("rotated 40 deg", {"axis": (0, 1, 0), "angle": 0.7}),
                          ("rotated + scaled 2x", {"axis": (0.3, 1, 0.2), "angle": 0.7,
                                                   "scale": 2.0}),
                          ("rotated + moved", {"axis": (0, 0, 1), "angle": -0.5,
                                               "position": (0.2, -0.1, 0.05)})):
        d, l, _ = form()
        l.lattice_gizmo(box=BOX, offsets=cage(**{"2_1_1": (0.4, 0, 0)}), **kwargs)
        scales.append(d.safe_step_scale())
        print(f"    {label:<22} step scale {scales[-1]:.4f}")
    if max(scales) - min(scales) > 1e-4:
        raise SystemExit("a rigid, uniformly scaled placement must not change the bound")
    print("    identical — the transform cannot make the field steeper")

    # --- one gesture, one undo ----------------------------------------------
    d, l, _ = form()
    d.enable_undo()
    fresh = d.eval(pts)
    touched = l.lattice_gizmo(box=BOX, offsets=cage(**{"2_1_1": (0.4, 0, 0),
                                                       "0_2_1": (-0.3, 0.2, 0)}))
    if np.allclose(d.eval(pts), fresh):
        raise SystemExit("the cage changed nothing")
    d.undo()
    if not np.allclose(d.eval(pts), fresh):
        raise SystemExit("one cage must be one undo step")
    print(f"\n  {len(touched)} items warped by one gesture, undone by one step")

    # --- dragging again replaces the cage, it does not stack another --------
    # Checked by what it MEANS rather than by counting deformers: a gesture
    # dragged through two frames must land where the second frame says, not
    # where both frames together would.
    twice, lt, _ = form()
    lt.lattice_gizmo(box=BOX, offsets=cage(**{"2_1_1": (0.35, 0, 0)}))
    lt.lattice_gizmo(box=BOX, offsets=cage(**{"2_1_1": (0.55, 0, 0)}))
    once, lo, _ = form()
    lo.lattice_gizmo(box=BOX, offsets=cage(**{"2_1_1": (0.55, 0, 0)}))
    drift = float(np.abs(twice.eval(pts) - once.eval(pts)).max())
    print(f"\n  dragged through two frames vs straight to the second: "
          f"worst difference {drift:.2e}")
    if drift > 1e-5:
        raise SystemExit("a second drag must replace the cage, not stack a second one")


if __name__ == "__main__":
    main()
