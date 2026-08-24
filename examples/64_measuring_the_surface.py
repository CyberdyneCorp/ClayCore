"""Asking the shape what it is: cavity, occlusion, thickness — and a bake cage.

Two gaps closed here, and they are the same gap twice.

`brush::mask_from_surface` derived curvature, cavity and convexity from the
field's Laplacian, had tests, and was reachable from NO HOST AT ALL — no C entry
point, no pyclay. It could also only ever return a lattice, which is the right
shape for masking and the wrong one for a baker, a vertex colour, or anything
asking about one point.

So the measure moved to a per-point form and the mask became one of its callers.
That is not tidying: it is what makes "the mask and the baked map agree about
this surface" a construction rather than a claim, since there is one
implementation and no second stencil to drift.

WHY THIS IS CHEAP ON A FIELD, and expensive in a mesh engine. Curvature here is
the LAPLACIAN, and its sign is unambiguous — for f = |p| - R it is 2/R at the
surface, POSITIVE for convex — so cavity and convexity are one subtraction
apart. A mesh has to estimate curvature from a vertex ring, a discrete
approximation with a valence-dependent error. The same argument runs for
occlusion: a field is marched directly, with no acceleration structure to build
and nothing to invalidate, and it measures the ACTUAL surface rather than a
tessellation of it.

AMBIENT OCCLUSION AND THICKNESS are new, and were deliberately absent before.
The header said why: both need rays cast from the surface, "a different cost
class and a different set of parameters (ray count, length, falloff), so they
are their own change rather than two more enumerators pretending to be as cheap
as the rest."

And `project` is the query a bake CAGE is: from a low-polygon point, find the
high-polygon surface within a distance — searching BOTH ways, because a cage
point sits inside the high-poly wherever the low-poly pinches and you cannot
know where that is.
"""

import numpy as np

import pyclay as clay

import _render as R

EYE, TARGET = (2.4, 1.5, 2.4), (0.0, 0.0, 0.0)


def surface_points(doc, count=2000, radius=1.4):
    """Points ON the surface, found by projecting inward from a sphere around it.

    Which is what `project` is for — and it doubles as this script's own
    fixture check: a measure taken off the surface describes nothing, and every
    assertion below would be comparing noise.
    """
    # A deterministic Fibonacci sphere, so the script's numbers do not wander.
    i = np.arange(count, dtype=np.float64) + 0.5
    phi = np.arccos(1.0 - 2.0 * i / count)
    theta = np.pi * (1.0 + 5.0**0.5) * i
    dirs = np.stack([np.cos(theta) * np.sin(phi), np.cos(phi),
                     np.sin(theta) * np.sin(phi)], axis=1)

    found = []
    for d in dirs:
        hit = doc.project(tuple(radius * d), tuple(-d), radius * 2.0)
        if hit is not None:
            found.append(hit["position"])
    return np.array(found, dtype=np.float32)


def main():
    R.banner("64 measuring the surface")

    # A shape with a real crease in it: two overlapping spheres. A TORUS is the
    # obvious cavity fixture and is WRONG for it — for major R and minor r the
    # mean curvature at the inner ring is 1/r - 1/(R - r), so the tube wins and
    # the ring reads CONVEX unless R < 2r. Worth stating, because that mistake
    # produces a cavity demo that measures nothing and looks fine.
    doc = clay.Document()
    layer = doc.add_sdf_layer("body")
    layer.add(clay.Sphere(r=0.5, position=(0.32, 0, 0)), color="#c0a98d")
    layer.add(clay.Sphere(r=0.5, position=(-0.32, 0, 0)), color="#c0a98d")

    R.render(doc, "64_shape.png", eye=EYE, target=TARGET,
             caption="two overlapping spheres — a genuine crease, unlike a torus")

    # --- the cage query, which is also how we get surface points ----------
    pts = surface_points(doc)
    if len(pts) < 500:
        raise SystemExit(
            f"only {len(pts)} of 2000 rays found the surface — every measure "
            "below would be sampling points that are not on it")
    on_surface = np.abs(doc.eval(pts))
    if on_surface.max() > 1e-2:
        raise SystemExit(
            f"projected points must be ON the surface; worst |f| = {on_surface.max():.4f}")
    print(f"  projected {len(pts)} points onto the surface, worst |field| "
          f"{on_surface.max():.2e}")

    # --- the crease, found by measurement rather than by knowing where it is --
    y_crease = float(np.sqrt(0.5**2 - 0.32**2))
    crease = np.array([[0.0, y_crease, 0.0], [0.0, -y_crease, 0.0]], dtype=np.float32)
    outer = np.array([[0.82, 0.0, 0.0], [-0.82, 0.0, 0.0]], dtype=np.float32)
    for name, probe in (("crease", crease), ("outer", outer)):
        if np.abs(doc.eval(probe)).max() > 1e-2:
            raise SystemExit(f"the {name} probe is not on the surface")

    print("\n  measured at the crease, and on the open outer side:\n")
    print(f"    {'':<14}{'crease':>10}{'outer':>10}")
    rows = [
        ("cavity", {"scale": 0.25}),
        ("convexity", {"scale": 0.25}),
        ("curvature", {"scale": 0.25}),
        ("occlusion", {"ray_length": 0.6, "ray_count": 48}),
        ("thickness", {"ray_length": 1.5}),
    ]
    got = {}
    for measure, params in rows:
        a = float(doc.measure(measure, crease, params).mean())
        b = float(doc.measure(measure, outer, params).mean())
        got[measure] = (a, b)
        print(f"    {measure:<14}{a:>10.3f}{b:>10.3f}")

    # The two claims worth asserting, because they are what the measures MEAN.
    if not got["cavity"][0] > got["cavity"][1]:
        raise SystemExit("the crease must read more concave than the open side")
    if not got["occlusion"][0] > got["occlusion"][1]:
        raise SystemExit(
            "the crease must read more OCCLUDED than the open side — this is "
            "occlusion, not lighting, so the greater number is the darker place")
    print("\n  the crease is more concave AND more occluded than the open side,")
    print("  which is the whole content of a cavity mask and an AO map.")

    # --- determinism, which is not negotiable here ------------------------
    params = {"ray_length": 0.6, "ray_count": 32, "seed": 7}
    first = doc.measure("occlusion", pts[:200], params)
    again = doc.measure("occlusion", pts[:200], params)
    if not np.array_equal(first, again):
        raise SystemExit(
            "occlusion must be bit-identical for the same seed: every other "
            "query in this library is, and a hemisphere sample is the first "
            "thing that could quietly break it")
    other = doc.measure("occlusion", pts[:200], {**params, "seed": 99})
    if np.array_equal(first, other):
        raise SystemExit(
            "a different seed must give a different sample set, or the check "
            "above is vacuous")
    print(f"\n  occlusion over {len(first)} points is bit-identical for seed 7, and "
          "differs\n  for seed 99 — so the reproducibility check is not vacuous")

    # --- the mask and the point cannot disagree ---------------------------
    # One implementation behind both. This is the assertion that would fail the
    # day a second stencil reappeared.
    cell = 0.02
    mask = doc.mask_from_surface("cavity", ((-1.0, -0.7, -0.7), (1.0, 0.7, 0.7)),
                                 cell_size=cell, band=0.05, params={"scale": 0.25})
    if mask.painted_count < 100:
        raise SystemExit("the cavity mask should have painted the crease")
    sample = pts[:300]
    at_points = doc.measure("cavity", sample, {"scale": 0.25, "h": cell})
    diffs = np.array([abs(mask.sample(tuple(float(c) for c in p)) - float(v))
                      for p, v in zip(sample, at_points)])
    agree = float((diffs < 0.05).mean())

    # THE BULK, NOT THE WORST CASE, and the reason is the design's own
    # documented cost rather than a fudge. `mask.sample` answers for the CELL a
    # point is in, while the per-point measure answers for the point; cavity at
    # this scale is saturated, 0 or 1, so a point in a cell that straddles the
    # crease boundary legitimately differs by the full 1.0. That is the
    # quantisation the lattice is documented to have.
    #
    # It still catches what it is for: a second stencil would disagree
    # EVERYWHERE, not only in the boundary cells.
    if agree < 0.9:
        raise SystemExit(
            f"only {agree:.0%} of points agree between the mask and the "
            "per-point measure — they are supposed to be one implementation")
    print(f"\n  the cavity MASK ({mask.painted_count} cells) and the per-point measure "
          f"agree on\n  {agree:.0%} of {len(sample)} surface points (median difference "
          f"{np.median(diffs):.4f}).")
    print("  The rest are cells straddling the crease boundary, where a saturated")
    print("  measure flips inside one cell — the lattice quantisation, by design.")

    # --- what a bake would actually do ------------------------------------
    # Given a cage point and a direction, find the high-poly surface and the
    # signed distance — which IS the height-map value.
    cage = np.array([[1.4, 0.0, 0.0]], dtype=np.float32)
    hit = doc.project(tuple(float(c) for c in cage[0]), (-1, 0, 0), 2.0)
    print(f"\n  a cage point at x=1.4 projects {hit['distance']:.3f} along -X onto "
          f"x={hit['position'][0]:.3f},")
    print("  and that signed distance is the height-map value — returned by the")
    print("  same call that found the point, so the sign cannot disagree.")

    # And the case a one-directional implementation silently misses.
    behind = doc.project((1.4, 0.0, 0.0), (1, 0, 0), 2.0)
    if behind is None or behind["distance"] >= 0:
        raise SystemExit(
            "projecting away from the surface must still find it, with a "
            "NEGATIVE distance: a cage point sits inside the high-poly wherever "
            "the low-poly pinches, and you cannot know where that is")
    print(f"  pointing the other way finds it at {behind['distance']:.3f} — negative, "
          "and the\n  reason 'both ways' is not a refinement.")


if __name__ == "__main__":
    main()
