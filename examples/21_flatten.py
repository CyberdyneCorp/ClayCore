"""Flattening a surface onto a plane.

Voxel layers have had `sculpt_flatten` all along. SDF layers had nothing but
the cut tool, which is global to its prism and has no falloff — so it can take
a slice off a whole model, but it cannot put a facet on one part of a shape and
leave the rest. That is the gap this closes, and it is the same asymmetry
`relax` closed for smoothing.

**This is not ZBrush's Clip brush, deliberately.** Clip moves vertices onto the
plane instead of deleting them. Ask what solid that bounds: the clamp map sends
everything past the plane onto it, and the image lies *on* the plane — measure
zero, no volume. As a solid, **Clip is exactly Trim**, which `Cut` already
does. Clip's distinctive look in ZBrush is the flattened polygons surviving as
a zero-thickness fin coplanar with the cut face, which is why Clip is so often
followed by Dynamesh. A field cannot represent a zero-thickness fin, and
manufacturing one to imitate a result people then delete would be building a
bug on purpose.

Three things are worth reading before looking.

**It is two-sided.** Material on the normal's side goes *and* hollows on the
other side fill, matching the voxel verb. Two representations sharing a verb's
name must share its meaning, or a document means something different depending
on which one it happens to be stored in. The third section shows a dent filling.

**It samples rather than editing a volume.** `relax` rewrites a volume's stored
samples in place, which is sound for it because smoothing moves the surface by
less than a cell — always inside the band. Flatten moves the surface by *many*
band widths, and a band cannot follow: once the surface has walked outside it
there are no samples where the surface now is, and the isosurface comes apart in
fragments. That was measured, not theorised — eight strokes moved a surface by
0.20 with a band of 0.05, and the render showed the wreckage.

So flatten blends the plane in *before* deciding which bricks to keep, and the
new band brackets the flattened surface by construction. Sampling from a
document builds a fresh volume that way; flattening an existing volume
resamples only the bricks the brush's ball reaches and re-decides which of them
store samples, which is what lets the facet land in bricks that held nothing.
Prefer the document: its field is exact everywhere, where a volume reports a
bound rather than a distance outside its own band.

**A brush can steepen the field, and this one says so.** `relax` could lean on
averaging being non-expansive. A blend under a weight that *varies across a
region* cannot:

    ∇[(1−w)·d + w·q] = (1−w)∇d + w∇q + (q − d)·∇w

The last term is the problem — a strong correction tapered over a short distance
makes the field steeper than it declares, and a field steeper than its declared
bound is one the raymarcher walks straight through. So the movement per pass is
bounded, the taper is widened when it is too narrow for it, and whatever is left
is **declared**: the volume reports a raised Lipschitz and the document's safe
step scale drops to match. The last section prints that.
"""

import numpy as np

import pyclay as clay

import _render as R


def lumpy_doc():
    """A ball with a bump on top and a bite out of one side: something above
    the plane to take down, and a hollow below it to fill."""
    doc = clay.Document()
    layer = doc.add_sdf_layer("l")
    layer.add(clay.Sphere(r=0.6), color="#b0784a")
    layer.add(clay.Sphere(r=0.22, position=(0, 0.62, 0)),
              blend=clay.Smooth(0.06), color="#b0784a")
    layer.add(clay.Sphere(r=0.2, position=(0.55, 0.1, 0)), op=clay.Op.SUBTRACT)
    return doc


def volume_doc(volume, colour="#b0784a"):
    doc = clay.Document()
    doc.add_sdf_layer("l").add(volume, color=colour)
    return doc


def faceted(doc, **kwargs):
    """Sample the document with the flatten applied, in one pass."""
    return clay.Volume.flattened_from(doc, cell=0.025, **kwargs)


def surface_height(volume, x=0.0, z=0.0):
    ys = np.arange(1.15, -1.15, -0.004, dtype=np.float32)
    pts = np.stack([np.full_like(ys, x), ys, np.full_like(ys, z)], axis=1)
    inside = np.nonzero(volume.eval(pts) < 0.0)[0]
    return float(ys[inside[0]]) if len(inside) else float("nan")


def main():
    R.banner("21 flatten — putting a facet on a surface")

    EYE, TARGET = (2.2, 1.6, 2.4), (0, 0.1, 0)
    HEIGHT = 0.55
    # A region is REQUIRED. Flatten is local: where its weight is 1 the result
    # IS the plane, so with no region at full strength it would not flatten the
    # ball, it would replace it with a half-space — a ball comes back as a box.
    PLANE = dict(plane_point=(0, HEIGHT, 0), plane_normal=(0, 1, 0),
                 centre=(0, 0.62, 0), region_radius=0.3, falloff=0.25)

    src = lumpy_doc()
    base = clay.Volume.from_document(src, cell=0.025)
    print(f"  sampled: {base.brick_count} bricks, {base.megabytes:.2f} MB")
    print(f"  the bump starts at y = {surface_height(base):.3f}; "
          f"flattening onto y = {HEIGHT}")

    # --- strength takes the surface to the plane ------------------------------
    tiles = [R.render_array(volume_doc(base), eye=EYE, target=TARGET, width=205, height=195)]
    labels = ["untouched"]
    heights = [surface_height(base)]
    for strength in (0.35, 0.7, 1.0):
        v = faceted(src, strength=strength, **PLANE)
        h = surface_height(v)
        heights.append(h)
        print(f"    strength {strength:<5} -> y = {h:.3f}")
        tiles.append(R.render_array(volume_doc(v), eye=EYE, target=TARGET,
                                    width=205, height=195))
        labels.append(f"strength {strength}")
    R.contact_sheet(tiles, "21_flatten_strokes.png", columns=4, caption=", ".join(labels))

    # --- which side it acts on: Flatten, hPolish, and the dual ----------------
    # Cutting WITHOUT filling is the whole hard-surface family — hPolish, Planar,
    # the Trim brushes. It is what leaves a crisp facet against untouched
    # surface, and filling the hollows beside a facet is what a polish must not
    # do. The three differ by one clamp on the blend term.
    #
    # Aimed at the DENTED side, where the difference is visible: the ball has a
    # bump above the plane and a hollow below it.
    SIDE = dict(plane_point=(0.5, 0, 0), plane_normal=(1, 0, 0),
                centre=(0.5, 0.1, 0), region_radius=0.34, falloff=0.25)
    in_dent = np.array([[0.45, 0.1, 0.0]], np.float32)

    def flank_x(volume, y, z=0.0):
        """Where the flank sits, marching in along -X."""
        xs = np.arange(1.2, -0.2, -0.003, dtype=np.float32)
        pts = np.stack([xs, np.full_like(xs, y), np.full_like(xs, z)], axis=1)
        inside = np.nonzero(volume.eval(pts) <= 0.0)[0]
        return float(xs[inside[0]]) if len(inside) else float("nan")

    modes = {m: faceted(src, mode=m, **SIDE) for m in ("two_sided", "cut", "fill")}
    print("  the flank profile — the plane is at x = 0.5, and y = 0.0-0.2 is the hollow:")
    print(f"    {'y':>6}{'source':>10}{'two_sided':>11}{'cut':>9}{'fill':>9}")
    for y in (-0.10, 0.00, 0.10, 0.20, 0.30):
        row = "".join(f"{flank_x(v, y):>{11 if k == 'two_sided' else 9}.3f}"
                      for k, v in modes.items())
        print(f"    {y:>6.2f}{flank_x(base, y):>10.3f}{row}")

    tiles = [R.render_array(volume_doc(modes[m]), eye=(2.6, 1.2, 1.6), target=(0.3, 0.1, 0),
                            width=205, height=195) for m in ("two_sided", "cut", "fill")]
    R.contact_sheet(tiles, "21_flatten_modes.png", columns=3,
                    caption="two_sided (ZBrush Flatten) planes the flank and closes the "
                            "hollow; cut (hPolish/Planar/Trim) planes the flank and leaves "
                            "the hollow as a crater; fill closes the hollow and leaves the "
                            "proud material standing as a rim")

    # cut must leave the hollow empty; fill must close it.
    if faceted(src, mode="cut", **SIDE).eval(in_dent)[0] <= 0.0:
        raise SystemExit("cut-only filled a hollow — that is Flatten, not hPolish")
    if faceted(src, mode="fill", **SIDE).eval(in_dent)[0] >= 0.0:
        raise SystemExit("fill-only did not close the hollow")
    # ...and cut still planes what stands proud.
    if abs(surface_height(faceted(src, mode="cut", **PLANE)) - HEIGHT) > 0.05:
        raise SystemExit("cut-only stopped planing the bump onto the plane")

    if not all(a >= b - 1e-3 for a, b in zip(heights, heights[1:])):
        raise SystemExit("more strength stopped moving the surface toward the plane")
    if abs(heights[-1] - HEIGHT) > 0.04:
        raise SystemExit("full strength did not put the surface on the plane")

    # --- the band follows the new surface ------------------------------------
    # The reason this samples instead of editing a volume in place: a band
    # cannot follow a surface that walks out of it.
    flat = faceted(src, **PLANE)
    on_facet = flat.has_samples_at((0.0, HEIGHT, 0.0))
    where_bump_was = flat.has_samples_at((0.0, 0.83, 0.0))
    print(f"  samples on the new facet: {on_facet}; "
          f"where the bump used to be: {where_bump_was}")
    if not on_facet or where_bump_was:
        raise SystemExit("the band is not bracketing the flattened surface")

    # --- it is two-sided ------------------------------------------------------
    # The dent in the +X side is a hollow below a plane placed outside it, so
    # flatten fills it. A subtract could only ever remove.
    in_dent = np.array([[0.45, 0.1, 0.0]], np.float32)
    before = float(base.eval(in_dent)[0])
    side = faceted(src, plane_point=(0.5, 0, 0), plane_normal=(1, 0, 0),
                   centre=(0.5, 0.1, 0), region_radius=0.3, falloff=0.25)
    after = float(side.eval(in_dent)[0])
    print(f"  the dent reads {before:+.4f} before and {after:+.4f} after — "
          f"filled, not deepened")
    if not after < 0.0:
        raise SystemExit("flatten did not fill a hollow below the plane")

    # --- it is a brush, not a filter -----------------------------------------
    tight = dict(PLANE, region_radius=0.28, falloff=0.18)
    patch = faceted(src, **tight)
    probes = np.random.default_rng(4).normal(size=(4000, 3))
    probes /= np.linalg.norm(probes, axis=1)[:, None]
    probes = (probes * 0.62).astype(np.float32)
    stored = np.array([base.has_samples_at(tuple(p)) for p in probes])
    delta = np.abs(patch.eval(probes) - base.eval(probes))
    under = stored & (probes[:, 1] > 0.45)
    away = stored & (probes[:, 1] < -0.3)
    print(f"  under the brush the field moved by up to {delta[under].max():.4f}; "
          f"on the far side by {delta[away].max():.4f}")
    if delta[away].max() > delta[under].max() * 0.05:
        raise SystemExit("the region is not containing the effect")

    R.contact_sheet(
        [R.render_array(volume_doc(base), eye=EYE, target=TARGET, width=310, height=290),
         R.render_array(volume_doc(patch, "#8d6a4f"), eye=EYE, target=TARGET,
                        width=310, height=290)],
        "21_flatten_region.png", columns=2,
        caption="before, and a facet under a brush on the top only — the rest of "
                "the form is untouched, which a Cut could not do")

    # --- what a region costs the raymarcher ----------------------------------
    # A region blends under a varying weight, which can steepen the field. It
    # declares what it measured rather than letting the marcher find out.
    print("  a tighter taper is steeper, and the step scale pays for it:")
    for falloff in (0.30, 0.15, 0.05):
        v = faceted(src, **dict(PLANE, region_radius=0.28, falloff=falloff))
        print(f"    falloff {falloff:<5} -> sample lipschitz {v.sample_lipschitz:.3f}, "
              f"step scale {volume_doc(v).safe_step_scale():.3f}")
    # ...and asking for no region at all is refused rather than silently
    # replacing the shape with a half-space.
    try:
        faceted(src, plane_point=(0, HEIGHT, 0), plane_normal=(0, 1, 0), region_radius=0.0)
        raise SystemExit("a region-less flatten should have been refused")
    except ValueError as e:
        print(f"    no region       -> refused: {str(e).split(':')[0]}")

    # And the consequence that matters: a ray still lands on the facet.
    doc = volume_doc(flat)
    result = doc.raycast_many(np.array([[0, 3.0, 0, 0, -1, 0]], np.float32))
    hit = bool(result["hit"][0])
    where = float(result["position"][0][1]) if hit else float("nan")
    print(f"  a ray marched down at it lands on the facet: {hit} (y = {where:.3f})")
    if not hit or abs(where - HEIGHT) > 0.05:
        raise SystemExit("the raymarcher no longer finds the flattened surface")

    # --- and the result is an ordinary item ----------------------------------
    carved = clay.Document()
    layer = carved.add_sdf_layer("l")
    layer.add(clay.Box(size=(0.9, 0.5, 0.9)), color="#7f8a94", rounding=0.04)
    layer.add(flat, op=clay.Op.SUBTRACT)
    R.render(carved, "21_flatten_carved.png", eye=(2.2, 1.7, 2.2), target=TARGET,
             caption="a flattened volume subtracted from a box — still just an item")

    R.export_model(volume_doc(flat), "21_flatten.ply", resolution=72, decimate=0.06)


if __name__ == "__main__":
    main()
