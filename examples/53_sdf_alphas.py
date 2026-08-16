"""Alphas on an SDF layer — and why the bound comes from steepness.

Detail work in every competing sculptor is alpha-driven: pores, fabric weave,
scales, stitching, panel lines. Until now this engine had `sculpt_carve_alpha`
on voxels and nothing equivalent on fields, which is the wrong asymmetry — the
SDF side is the *non-destructive* one, so restricting the one detailing
technique artists actually use to the baked representation makes you choose
between detail and re-editability.

**An alpha is a deformer, not a primitive**, and that is the design rather than
an implementation detail. A primitive shaped like the stamp would *add* material
in the stamp's shape. That is not what an alpha does: it modulates a surface
that is already there — pores in existing skin — which is a distance offset
under finite support. The engine already had that shape twice (`noise` offsets a
whole item, `blob` offsets it under a radial falloff); an alpha is `blob` with
the fractal replaced by a lookup into your array.

**The engine decodes no images.** You hand over `height x width` samples in
[0, 1]. A library that compiles to CPU, Metal, CUDA, OpenCL and Vulkan has no
business linking a PNG decoder, and a host with an alpha has already paid that
cost.

**The interesting part is the Lipschitz bound**, because it is where a stamp
stops being free. Every deformer here declares how far it can stretch space, and
the raymarcher's step size is *derived* from that rather than tuned. An alpha's
offset is `amplitude * falloff(p) * stamp(u, v)`, so its bound needs the stamp's
own steepness — and the honest measure of that is the largest difference between
**adjacent samples** over the world distance between them, not the largest
sample value.

That distinction is the whole content of this script's third section. A stamp of
all ones has the largest possible *values* and is perfectly flat: it displaces
rigidly and adds no steepness at all. A bound taken from magnitudes would charge
it as though it were the steepest stamp there is.
"""

import numpy as np

import pyclay as clay

import _render as R

R_SPHERE = 0.62
# Framed with margin rather than tight. At the tighter distance the sphere
# exactly filled the frame, so the relief — the whole subject — was clipped off
# at the tile edge.
EYE, TARGET = (1.89, 1.23, 2.09), (0.0, 0.0, 0.35)


def stamped(samples, amplitude=0.09, extent=0.85, radius=0.62, centre=None, tangent=(0, 1, 0)):
    """A sphere with one alpha on its +Z pole."""
    doc = clay.Document()
    prim = clay.Sphere(r=R_SPHERE)
    if samples is not None:
        prim = prim.alpha(np.ascontiguousarray(samples, dtype=np.float32),
                          centre=centre or (0.0, 0.0, R_SPHERE),
                          direction=(0, 0, 1), tangent=tangent,
                          extent=extent, radius=radius, amplitude=amplitude)
    doc.add_sdf_layer("l").add(prim, color="#c98f63")
    return doc


def surface_along_z(doc, lo=0.0, hi=3.0, steps=70):
    """Where the surface crosses +Z, by bisection on the field."""
    for _ in range(steps):
        mid = 0.5 * (lo + hi)
        if doc.eval(np.array([[0.0, 0.0, mid]], dtype=np.float32))[0] < 0.0:
            lo = mid
        else:
            hi = mid
    return 0.5 * (lo + hi)


# --- the stamps ---------------------------------------------------------------
# Built rather than loaded, so the script has no asset dependency and the
# frequency content of each one is something you can read off the code.

def pores(n=96, seed=53):
    """Scattered dimples — the classic skin alpha."""
    rng = np.random.default_rng(seed)
    y, x = np.mgrid[0:n, 0:n].astype(np.float32)
    out = np.zeros((n, n), dtype=np.float32)
    for _ in range(70):
        cx, cy = rng.uniform(0, n, size=2)
        r = rng.uniform(n * 0.02, n * 0.05)
        d = np.sqrt((x - cx) ** 2 + (y - cy) ** 2) / r
        out = np.maximum(out, np.clip(1.0 - d, 0.0, 1.0) ** 2)
    return out


def weave(n=96, period=12):
    """Fabric: two crossed sinusoids, smooth and low-frequency."""
    y, x = np.mgrid[0:n, 0:n].astype(np.float32)
    return (0.5 + 0.5 * np.sin(2 * np.pi * x / period)) * \
           (0.5 + 0.5 * np.sin(2 * np.pi * y / period))


def scales(n=96, rows=7):
    """Overlapping arcs, offset row to row."""
    y, x = np.mgrid[0:n, 0:n].astype(np.float32)
    step = n / rows
    row = np.floor(y / step)
    u = (x + np.where(row % 2 > 0, step * 0.5, 0.0)) % step
    v = y % step
    d = np.sqrt((u - step * 0.5) ** 2 + (v - step) ** 2) / (step * 0.6)
    return np.clip(1.0 - np.abs(d - 0.75) * 4.0, 0.0, 1.0).astype(np.float32)


def rivets(n=96, count=5):
    """A hard-surface stamp: a grid of raised studs."""
    y, x = np.mgrid[0:n, 0:n].astype(np.float32)
    step = n / count
    u, v = (x % step) - step * 0.5, (y % step) - step * 0.5
    d = np.sqrt(u * u + v * v) / (step * 0.22)
    return np.clip(1.0 - d, 0.0, 1.0).astype(np.float32) ** 0.6


def main():
    R.banner("53 sdf alphas — the stamp, and what its steepness costs")

    plain = stamped(None)
    bare = surface_along_z(plain)
    print(f"  a bare sphere: surface at z={bare:.4f}, step scale "
          f"{plain.safe_step_scale():.4f}")

    # --- an unused stamp is exactly the item --------------------------------
    rng = np.random.default_rng(53)
    pts = rng.uniform(-1.4, 1.4, size=(512, 3)).astype(np.float32)
    zeroed = stamped(np.zeros((32, 32), dtype=np.float32))
    worst = float(np.abs(plain.eval(pts) - zeroed.eval(pts)).max())
    print(f"\n  an ALL-ZERO stamp against no stamp at all: worst {worst:.2e}, "
          f"step scale {zeroed.safe_step_scale():.4f}")
    if worst != 0.0 or zeroed.safe_step_scale() != plain.safe_step_scale():
        raise SystemExit("an unused stamp must be exactly the item, and cost nothing")

    # --- white is raised, black carves ---------------------------------------
    # Worth demonstrating because it is a footgun if it is wrong. A deformer's
    # offset is ADDED to the distance, where positive means further away — so
    # the kernel flips the sign once, and `amplitude` means what an artist
    # expects it to mean.
    flat = np.ones((32, 32), dtype=np.float32)
    up = surface_along_z(stamped(flat, amplitude=0.12))
    down = surface_along_z(stamped(flat, amplitude=-0.12))
    print(f"\n  amplitude +0.12: the surface reaches z={up:.4f}  (raised)")
    print(f"  amplitude -0.12: the surface reaches z={down:.4f}  (carved)")
    if not (up > bare > down):
        raise SystemExit("white must be raised and black carved")

    # --- outside the radius, nothing at all ----------------------------------
    marked = stamped(pores(), amplitude=0.09)
    outside = np.array([[0, 0, -1.0], [1.0, 0, 0], [0, -1.0, 0]], dtype=np.float32)
    if not np.array_equal(plain.eval(outside), marked.eval(outside)):
        raise SystemExit("material outside the radius must be untouched exactly")
    print("\n  material outside the radius is untouched EXACTLY — which is what")
    print("  makes this a brush rather than a modifier, and what keeps the")
    print("  influence bound tight enough for culling to be worth anything.")

    # --- the bound comes from STEEPNESS, not from values ---------------------
    print("\n  what a stamp costs in step scale:")
    print(f"    {'stamp':<22}{'peak':>7}{'steepest':>10}{'step scale':>13}")
    cases = [
        ("all ones (flat)", flat),
        ("weave (smooth)", weave()),
        ("scales", scales()),
        ("pores", pores()),
        ("rivets", rivets()),
        ("checkerboard", (np.indices((32, 32)).sum(0) % 2).astype(np.float32)),
    ]
    scores = {}
    for name, s in cases:
        doc = stamped(s, amplitude=0.09)
        scores[name] = doc.safe_step_scale()
        # The two numbers the bound is built from, computed here the same way
        # the engine computes them.
        steep = max(float(np.abs(np.diff(s, axis=0)).max()),
                    float(np.abs(np.diff(s, axis=1)).max()))
        print(f"    {name:<22}{float(s.max()):>7.2f}{steep:>10.2f}{scores[name]:>13.4f}")

    # The claim, checked rather than asserted: the flat stamp has the LARGEST
    # possible values and costs the least, because it is not steep.
    if scores["all ones (flat)"] < max(scores[n] for n, _ in cases if n != "all ones (flat)"):
        raise SystemExit("a flat stamp must be the cheapest, whatever its values")
    if scores["checkerboard"] > scores["weave (smooth)"]:
        raise SystemExit("a high-frequency stamp must cost more than a smooth one")
    print("    The flat stamp has the LARGEST values and the LOWEST cost. A bound")
    print("    taken from magnitudes would have charged it as the steepest there is.")

    # --- ...and it is a WORLD slope ------------------------------------------
    # The same samples over a larger footprint are a gentler surface. A bound
    # over raw sample differences would miss this and charge both the same.
    print("\n  the same stamp, spread over different extents:")
    print(f"    {'extent':>8}{'step scale':>13}")
    previous = None
    for extent in (0.3, 0.6, 1.2, 2.4):
        doc = stamped(scales(), amplitude=0.09, extent=extent)
        print(f"    {extent:>8.2f}{doc.safe_step_scale():>13.4f}")
        if previous is not None and doc.safe_step_scale() <= previous:
            raise SystemExit("spreading the same relief wider must cost less")
        previous = doc.safe_step_scale()

    # --- the pictures ---------------------------------------------------------
    tiles = []
    for name, s in cases[:5]:
        doc = stamped(s, amplitude=0.075)
        tiles.append(R.render_array(doc, eye=EYE, target=TARGET, width=250, height=250,
                                    colors_from_field=True))
    R.contact_sheet(tiles, "53_sdf_alphas.png", columns=5,
                    caption=", ".join(n for n, _ in cases[:5]))

    # --- placing one where a user clicked ------------------------------------
    # The frame is derived by the library rather than by the host, so a preview
    # and its commit cannot disagree by a rotation. Any rough "up" works: it is
    # projected into the stamp's plane.
    print("\n  a stamp placed from a surface hit:")
    # One ray, packed origin-then-direction the way raycast_many takes them.
    ray = np.array([[0.0, 0.0, 2.0, 0.0, 0.0, -1.0]], dtype=np.float32)
    result = plain.raycast_many(ray)
    if not result["hit"][0]:
        raise SystemExit("the probe ray missed the sphere")
    point = np.asarray(result["position"])[0]
    normal = np.asarray(result["normal"])[0]
    print(f"    hit at {tuple(round(float(v), 3) for v in point)}, "
          f"normal {tuple(round(float(v), 3) for v in normal)}")
    placed = stamped(rivets(), amplitude=0.075, centre=tuple(float(v) for v in point),
                     tangent=tuple(float(v) for v in normal))  # deliberately parallel
    if not np.isfinite(placed.safe_step_scale()) or placed.safe_step_scale() <= 0.0:
        raise SystemExit("a tangent parallel to the normal must not collapse the frame")
    print(f"    a tangent parallel to the normal still gives a usable frame "
          f"(step scale {placed.safe_step_scale():.4f})")

    # --- and it survives the file --------------------------------------------
    doc = stamped(scales(), amplitude=0.09)
    path = R.output_path("53_sdf_alphas.clayspace")
    doc.save(str(path))
    back = clay.load(str(path))
    if not np.array_equal(doc.eval(pts), back.eval(pts)):
        raise SystemExit("a stamp must survive the file exactly")
    if back.safe_step_scale() != doc.safe_step_scale():
        raise SystemExit("the reloaded bound must match")
    print("\n  saved and reloaded: the field is identical and so is the bound,")
    print("  which is RECOMPUTED on load rather than read — so a hand-edited")
    print("  file cannot claim a looser bound than its samples deserve.")

    # NO COMMITTED MODEL here, deliberately. Alpha relief is high-frequency by
    # nature: this document meshes to 378k triangles at resolution 128, and the
    # gallery's 400 KiB budget puts the export somewhere around resolution 32 —
    # which smooths away the very thing the file would be showing. A committed
    # PLY of a smooth blob labelled "alpha detail" is worse than no PLY. The
    # renders above carry the demonstration, and the numbers carry the claims.


if __name__ == "__main__":
    main()
