"""Relaxing a surface — the last of the core sculpting brushes.

Voxel layers have had smoothing all along. SDF layers had nothing, and the
only route was the one-way voxel bridge.

**The design question was open, and settling it is most of this row.** Three
routes were on the table:

* **Round-trip through a sampled field** — sample, smooth the samples, hand
  back a volume. Gives a bake, not a live edit.
* **A field-space local re-blend** — no prerequisite, but it changes what an
  edit list *means*: every item's contribution gets reweighted by its
  neighbours, so the list stops being a list of shapes and becomes shapes plus
  a rule about how they interact. That reaches undo, picking, serialization and
  the C ABI — and it still could not smooth a bump in the middle of *one* item,
  because there is no second item to reweight against.
* **Mesh-space extract, smooth, re-import** — the first route with a meshing
  round trip in the middle. Loses the interior, costs more, buys nothing.

The first one, and the honest cost is stated below rather than hidden:
**relax bakes.** What comes back is a sampled volume, not the edit list that
went in.

The roadmap warned that convolving a distance field "breaks the distance
property the evaluator depends on". That is half right, and the wrong half is
the one that decides whether this is safe. Convolution destroys **exactness** —
the result no longer reports the true distance to its own surface. It cannot
break the **Lipschitz bound**, because an average cannot vary faster than the
thing it averages:

    |f̄(x) − f̄(y)| ≤ Σ wᵢ |f(x+dᵢ) − f(y+dᵢ)| ≤ |x − y|

and a field whose slope is bounded by one is automatically a conservative bound
on the distance to its own zero set: if z is the nearest zero to x then
|f(x)| = |f(x) − f(z)| ≤ |x − z|. So sphere tracing on a relaxed field cannot
overstep. The third section measures that slope before and after.
"""

import numpy as np

import pyclay as clay

import _render as R


def bumpy_doc(amplitude=0.05, frequency=15.0):
    """A sphere with a ripple: a feature small enough for smoothing to remove,
    on a shape large enough to survive it.

    Built as many small spheres rather than with a noise field, because the
    engine has no noise primitive and an example should use what exists.
    """
    doc = clay.Document()
    layer = doc.add_sdf_layer("l")
    layer.add(clay.Sphere(r=0.66), color="#b0784a")
    rng = np.random.default_rng(5)
    for _ in range(90):
        d = rng.normal(size=3)
        d /= np.linalg.norm(d)
        layer.add(clay.Sphere(r=0.09, position=tuple(d * 0.68)),
                  blend=clay.Smooth(0.04), color="#b0784a")
    return doc


def volume_doc(volume, colour="#b0784a"):
    doc = clay.Document()
    doc.add_sdf_layer("l").add(volume, color=colour)
    return doc


def ripple(volume, r=0.7, samples=2000):
    """How far the surface wanders, measured on a sphere through it."""
    rng = np.random.default_rng(3)
    d = rng.normal(size=(samples, 3))
    d /= np.linalg.norm(d, axis=1)[:, None]
    pts = (d * r).astype(np.float32)
    keep = np.array([volume.has_samples_at(tuple(p)) for p in pts])
    return float(np.abs(volume.eval(pts)[keep]).std())


def steepest_slope(volume, h=1e-3):
    """The Lipschitz bound the whole design rests on, measured.

    The whole finite-difference stencil has to land where the volume stores
    samples. A stencil straddling the boundary between sampled space and the
    bounded space beyond it measures that seam — where the field jumps from an
    interpolated distance to a flat lower bound — and reports a slope in the
    tens, which says nothing about the interpolant.
    """
    rng = np.random.default_rng(9)
    pts = rng.uniform(-0.95, 0.95, size=(3000, 3)).astype(np.float32)
    worst = 0.0
    for axis in range(3):
        step = np.zeros(3, np.float32)
        step[axis] = h
        ok = np.array([volume.has_samples_at(tuple(p))
                       and volume.has_samples_at(tuple(p + step))
                       and volume.has_samples_at(tuple(p - step)) for p in pts])
        if not ok.any():
            continue
        here = pts[ok]
        g = (volume.eval(here + step) - volume.eval(here - step)) / (2 * h)
        worst = max(worst, float(np.abs(g).max()))
    return worst


def main():
    R.banner("20 relax — smoothing an SDF surface")

    EYE, TARGET = (2.2, 1.7, 2.4), (0, 0, 0)

    src = bumpy_doc()
    rough = clay.Volume.from_document(src, cell=0.02)
    print(f"  sampled the bumpy shape: {rough.brick_count} bricks, {rough.megabytes:.2f} MB")

    # --- more smoothing, less bumpy ------------------------------------------
    tiles = [R.render_array(volume_doc(rough), eye=EYE, target=TARGET, width=205, height=195)]
    labels = ["not relaxed"]
    amplitudes = [ripple(rough)]
    print("  surface roughness against the number of passes:")
    print(f"    {0:>2} passes -> {amplitudes[0]:.4f}")
    # A kernel of 5 cells against bumps ~4 cells across: smaller and the
    # kernel cannot reach across a bump, so it smooths the field without
    # moving the feature.
    for passes in (1, 4, 12):
        smoothed = rough.relaxed(radius_cells=5, iterations=passes)
        amplitudes.append(ripple(smoothed))
        print(f"    {passes:>2} passes -> {amplitudes[-1]:.4f}")
        tiles.append(R.render_array(volume_doc(smoothed), eye=EYE, target=TARGET,
                                    width=205, height=195))
        labels.append(f"{passes} pass{'es' if passes > 1 else ''}")
    R.contact_sheet(tiles, "20_relax_passes.png", columns=4, caption=", ".join(labels))
    if not all(a > b for a, b in zip(amplitudes, amplitudes[1:])):
        raise SystemExit("more passes stopped making the surface smoother")

    # --- the bound that sphere tracing depends on survives -------------------
    smoothed = rough.relaxed(radius_cells=5, iterations=4)
    before, after = steepest_slope(rough), steepest_slope(smoothed)
    print(f"  steepest slope: {before:.3f} before, {after:.3f} after — "
          f"averaging cannot raise it")
    if after > before + 0.05:
        raise SystemExit("relaxing made the field steeper, which it cannot do")

    # And the consequence: a ray still lands on the surface rather than
    # stepping through it.
    doc = volume_doc(smoothed)
    ray = np.array([[0, 0, 3.0, 0, 0, -1]], np.float32)
    result = doc.raycast_many(ray)
    hit = bool(result["hit"][0])
    where = float(np.linalg.norm(result["position"][0])) if hit else -1.0
    print(f"  a ray marched at it still lands on the surface: {hit} (at radius {where:.3f})")
    if not hit or not (0.5 < where < 0.9):
        raise SystemExit("the raymarcher no longer finds the relaxed surface")

    # --- it is a brush, not only a filter ------------------------------------
    # Relax with a region: one patch smoothed, the rest untouched.
    patch = rough.relaxed(radius_cells=5, iterations=10,
                          centre=(0.0, 0.68, 0.0), region_radius=0.35, falloff=0.2)
    probes = np.random.default_rng(17).normal(size=(4000, 3))
    probes /= np.linalg.norm(probes, axis=1)[:, None]
    probes = (probes * 0.68).astype(np.float32)
    stored = np.array([rough.has_samples_at(tuple(p)) for p in probes])
    delta = np.abs(patch.eval(probes) - rough.eval(probes))
    inside = stored & (probes[:, 1] > 0.5)     # under the brush
    outside = stored & (probes[:, 1] < -0.3)   # the far side
    print(f"  a region-limited relax changed the patch by up to {delta[inside].max():.4f} "
          f"and the far side by {delta[outside].max():.4f}")
    if delta[outside].max() > delta[inside].max() * 0.05:
        raise SystemExit("the region is not containing the effect")

    # Looked at from above, because that is where the brush was aimed.
    ABOVE = (0.9, 2.7, 1.1)
    R.contact_sheet(
        [R.render_array(volume_doc(rough), eye=ABOVE, target=TARGET, width=310, height=290),
         R.render_array(volume_doc(patch, "#8d6a4f"), eye=ABOVE, target=TARGET,
                        width=310, height=290)],
        "20_relax_region.png", columns=2,
        caption="before, and relaxed under a brush on the top only — the taper is "
                "widened if it is too narrow to hide the seam the kernel makes")

    # --- what it costs -------------------------------------------------------
    # Relax BAKES. The document that comes back is a volume, not the edit list
    # that went in, and that is inherent: a general relax has to smooth a bump
    # in the middle of one item, which no reweighting of an edit list expresses.
    # 91 items went in — a sphere and ninety bumps, each with an editable
    # radius and position. One volume comes out, at the resolution chosen
    # above, and none of those parameters survives.
    print(f"  {91} items went in; one volume came out at cell {rough.cell_size:.3f} "
          f"({rough.megabytes:.2f} MB) — relax BAKES, and that is the trade")

    # --- and the result is an ordinary item ----------------------------------
    carved = clay.Document()
    layer = carved.add_sdf_layer("l")
    layer.add(clay.Box(size=(0.85, 0.85, 0.85)), color="#7f8a94", rounding=0.05)
    layer.add(smoothed, op=clay.Op.SUBTRACT)
    R.render(carved, "20_relax_carved.png", eye=(2.2, 1.7, 2.2), target=TARGET,
             caption="a relaxed volume subtracted from a box — still just an item")

    R.export_model(volume_doc(smoothed), "20_relax.ply", resolution=96, decimate=0.1)


if __name__ == "__main__":
    main()
