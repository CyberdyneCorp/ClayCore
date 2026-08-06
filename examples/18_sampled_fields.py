"""Sampled fields: baking a field onto a grid, and using it as an item.

Everything else in this gallery is a field defined by a *formula* — evaluated
exactly, at any point, for free. A sampled field is the other kind: values
measured onto a grid and interpolated between. It exists because some fields
have no formula. An imported mesh is the case that matters, and it is the next
row; this one builds the container and proves it out on a field we already
know, so that a disagreement here is a sampling bug rather than a mesh bug.

Three things are worth reading before looking.

**Storage follows the surface, not the region.** Only bricks that straddle the
band store samples. The rest record which SIDE they are on, one integer each —
which is the thing a band alone could not do, and what keeps the field correct
far from the surface. The cost is O(area), not O(volume), and the second
section prints that against what a dense grid would have cost.

The plan for this row assumed the opposite and budgeted for a resource handle
outside the tape, on the grounds that a 256³ volume meant re-uploading tens of
megabytes per stroke. That was arithmetic on a DENSE grid. A narrow band at the
same resolution is a couple of megabytes, so the volume rides in the tape's
blob like every other out-of-line payload and all four backends get it with no
new plumbing.

**It is a bound, and the two halves of that are different.** Where the volume
has samples the value is a trilinear interpolation, and interpolating a convex
field *overshoots* — so it is accurate to the sampling but is **not** a lower
bound there. Where it has none, the value is a genuine lower bound. The fourth
section checks both halves, because only the second one is what stops a
raymarcher stepping through a surface.

**Accuracy is a control, not a hope.** The error inside the band shrinks with
the cell size, so `cell` is the dial. The third section measures it.
"""

import numpy as np

import pyclay as clay

import _render as R


def source_doc():
    """The field being sampled: a shape with thin parts and a hole, so that a
    cell size too coarse to hold it shows up as something you can see."""
    doc = clay.Document()
    layer = doc.add_sdf_layer("l")
    layer.add(clay.Torus(R=0.75, r=0.22), color="#b0784a")
    layer.add(clay.Sphere(r=0.45), blend=clay.Smooth(0.12), color="#b0784a")
    layer.add(clay.Cylinder(h=1.4, r=0.16), op=clay.Op.SUBTRACT)
    return doc


def volume_doc(volume, colour="#b0784a"):
    doc = clay.Document()
    doc.add_sdf_layer("l").add(volume, color=colour)
    return doc


def main():
    R.banner("18 sampled fields — a field measured onto a grid")

    EYE, TARGET = (2.2, 1.7, 2.4), (0, 0, 0)
    src = source_doc()

    # --- the same shape, formula against samples -----------------------------
    print("  baking the same field at four cell sizes:")
    tiles = [R.render_array(src, eye=EYE, target=TARGET, width=205, height=195)]
    labels = ["exact (a formula)"]
    for cell in (0.10, 0.05, 0.025):
        v = clay.Volume.from_document(src, cell=cell)
        print(f"    cell {cell:<6} {v.brick_count:>5} bricks  {v.megabytes:6.2f} MB")
        tiles.append(R.render_array(volume_doc(v), eye=EYE, target=TARGET,
                                    width=205, height=195))
        labels.append(f"sampled at {cell}")
    R.contact_sheet(tiles, "18_sampled_bake.png", columns=4, caption=", ".join(labels))

    # --- storage follows the surface -----------------------------------------
    # The same shape in a region eight times the volume. A dense grid would
    # grow with the box; a band grows with the surface.
    cell = 0.04
    tight = clay.Volume.from_document(src, cell=cell)
    (lo, hi) = tight.bounds
    span = max(hi[i] - lo[i] for i in range(3))
    roomy = clay.Volume.from_document(
        src, cell=cell,
        bounds=((-span, -span, -span), (span, span, span)))

    dense = (2 * span / cell) ** 3 * 4 / (1024 * 1024)  # float32, if it were dense
    print(f"  in a region {(2 * span / (hi[0] - lo[0])) ** 3:.1f}x the volume:")
    print(f"    tight {tight.megabytes:6.2f} MB   roomy {roomy.megabytes:6.2f} MB"
          f"   dense would be {dense:8.2f} MB")
    if roomy.megabytes > tight.megabytes * 3:
        raise SystemExit("storage is growing with the region, not the surface")
    if roomy.megabytes * 10 > dense:
        raise SystemExit("the band is no longer a saving over a dense grid")

    # --- accuracy is a dial --------------------------------------------------
    # Measured only where the volume actually stores samples: outside that it
    # is deliberately a loose bound, and averaging the two would report a
    # meaningless number that got better as the band got thinner.
    rng = np.random.default_rng(7)
    probes = rng.uniform(-1.0, 1.0, size=(4000, 3)).astype(np.float32)
    truth = src.eval(probes)
    near = np.abs(truth) < 0.25
    print("  worst error inside the band, against the cell size:")
    worst = []
    for cell in (0.16, 0.08, 0.04, 0.02):
        v = clay.Volume.from_document(src, cell=cell)
        stored = np.array([v.has_samples_at(tuple(p)) for p in probes])
        where = near & stored
        err = float(np.abs(v.eval(probes)[where] - truth[where]).max())
        worst.append(err)
        print(f"    cell {cell:<6} -> {err:.4f}")
    if not all(a > b for a, b in zip(worst, worst[1:])):
        raise SystemExit("a finer cell no longer buys accuracy — the dial is broken")

    # --- what is guaranteed, and what is not ---------------------------------
    v = clay.Volume.from_document(src, cell=0.04)
    stored = np.array([v.has_samples_at(tuple(p)) for p in probes])
    got = v.eval(probes)

    # Where there are NO samples the value must never exceed the truth in
    # magnitude, or a raymarcher steps straight through the surface.
    empty = ~stored
    over = ((truth[empty] > 0) & (got[empty] > truth[empty] + 1e-4)) | \
           ((truth[empty] < 0) & (got[empty] < truth[empty] - 1e-4))
    print(f"  of {int(empty.sum())} probes in sample-free space, "
          f"{int(over.sum())} overstate the distance")
    if over.any():
        raise SystemExit("a sample-free brick is overstating its distance")

    # Where there ARE samples it is an interpolation, and interpolating a
    # convex field overshoots. This is not a defect to be asserted away; it is
    # the reason the guarantee above is stated only for the empty case.
    inside = stored & (truth > 0)
    overshoot = float((got[inside] - truth[inside]).max())
    print(f"  where it interpolates, it overshoots by up to {overshoot:.4f} "
          f"({overshoot / 0.04:.2f} cells) — bounded, and not claimed away")

    print(f"  the document's safe step scale: {volume_doc(v).safe_step_scale():.3f}")

    # --- a volume is an ordinary item ----------------------------------------
    doc = clay.Document()
    layer = doc.add_sdf_layer("l")
    layer.add(clay.Box(size=(1.0, 0.55, 1.0)), color="#7f8a94", rounding=0.06)
    layer.add(clay.Volume.from_document(src, cell=0.03, position=(0, 0.1, 0)),
              op=clay.Op.SUBTRACT)
    R.render(doc, "18_sampled_carved.png", eye=(2.4, 1.9, 2.4), target=TARGET,
             caption="a sampled field subtracted from a box, and moved while it was at it")

    # --- it survives a save --------------------------------------------------
    # The volume goes into the document, not beside it: a .clayspace that
    # referenced an external grid would be a file that stopped working when
    # something else moved.
    baked = volume_doc(clay.Volume.from_document(src, cell=0.05))
    path = R.output_path("18_sampled.clayspace")
    baked.save(path)
    back = clay.load(path)
    probe = rng.uniform(-1.2, 1.2, size=(500, 3)).astype(np.float32)
    if not np.allclose(baked.eval(probe), back.eval(probe), atol=1e-5):
        raise SystemExit("the volume did not survive the round trip")
    print(f"  saved and reloaded {len(open(path, 'rb').read()) / 1024:.0f} KB, "
          f"field identical")

    R.export_model(volume_doc(clay.Volume.from_document(src, cell=0.03)),
                   "18_sampled.ply", resolution=96, decimate=0.1)


if __name__ == "__main__":
    main()
