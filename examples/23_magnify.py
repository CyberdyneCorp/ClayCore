"""Magnify and pinch — one deformation, one signed strength.

Maxon's own page has it: *"Magnify: pushes vertices away from cursor; inverse of
Pinch."* They are not two brushes that happen to be related, they are the same
radial scale with the sign flipped, so this exposes one parameter rather than
two verbs. Pinch is what gives a hard edge its crease; magnify is what swells a
feature without moving it.

The row was scoped as `add-magnify-blob`, and **Blob was carved out with a
reason.** Its character is an *irregular* surface response — that is the whole
point of the brush and why it reads organic rather than machined. The nearest
thing here is the `displace` deformer, whose sine is regular by construction: it
would give an even corrugation, which is precisely what Blob is not. Noise is a
real decision — which noise, whether it lives in the kernel dialect so all four
backends agree bit-for-bit, how a seed reaches the tape — and it deserves its
own row rather than being smuggled in behind a brush.

Two things are worth reading before looking.

**The centre of the scale is its fixed point.** This is the surprise, and it
caught the tests before it caught anything else: a radial scale about a point on
the surface bulges the neighbourhood *around* that point and leaves the point
itself exactly where it was. A probe running straight through the centre
measures the one place the deformation cannot move. The second section shows
both readings.

**Scaling space costs the raymarcher.** A radial scale is not distance
preserving, so the field stops being exact and its slope grows — most where the
falloff is steepest, which is why the easing curve enters the bound. The tape
carries it and the safe step scale drops accordingly, the same treatment `grab`
and `pose` get. The last section prints the cost against the strength.
"""

import numpy as np

import pyclay as clay

import _render as R


def ridged(magnify=None):
    """A rounded box with a bar across it: an edge for pinch to crease, and a
    mass for magnify to swell."""
    doc = clay.Document()
    layer = doc.add_sdf_layer("l")
    body = clay.RoundBox(size=(0.55, 0.42, 0.55), r=0.14)
    if magnify is not None:
        body.magnify(**magnify)
    layer.add(body, color="#b0784a")
    return doc


def surface_x(doc, y=0.0, z=0.0):
    xs = np.arange(0.0, 2.5, 0.002, dtype=np.float32)
    pts = np.stack([xs, np.full_like(xs, y), np.full_like(xs, z)], axis=1)
    out = np.nonzero(doc.eval(pts) > 0.0)[0]
    return float(xs[out[0]]) if len(out) else float("nan")


def main():
    R.banner("23 magnify and pinch — one deformation, one signed strength")

    EYE, TARGET = (2.3, 1.5, 2.3), (0, 0, 0)

    # --- one parameter, both directions --------------------------------------
    print("  the same deformer, swept through zero:")
    tiles, labels = [], []
    for strength in (-0.6, -0.3, 0.0, 0.45):
        doc = ridged(dict(center=(0, 0, 0), radius=0.95, strength=strength))
        x = surface_x(doc)
        name = "pinch" if strength < 0 else ("none" if strength == 0 else "magnify")
        print(f"    strength {strength:+.2f} ({name:<7}) -> surface at x = {x:.3f}, "
              f"step scale {doc.safe_step_scale():.3f}")
        tiles.append(R.render_array(doc, eye=EYE, target=TARGET, width=205, height=195))
        labels.append(f"{strength:+.2f}")
    R.contact_sheet(tiles, "23_magnify_sweep.png", columns=4,
                    caption="strength " + ", ".join(labels) +
                            " — negative gathers, positive swells")

    widths = [surface_x(ridged(dict(center=(0, 0, 0), radius=0.95, strength=s)))
              for s in (-0.6, -0.3, 0.0, 0.45)]
    if not all(a < b for a, b in zip(widths, widths[1:])):
        raise SystemExit("strength no longer moves the surface monotonically")

    # --- the centre is the fixed point ---------------------------------------
    # The surprise, and the thing that caught the tests first: a scale about a
    # point on the surface leaves that point alone and bulges around it.
    plain = ridged()
    # Centred exactly ON the surface, measured rather than assumed — `size` is
    # the full extent here, so the right face is nearer than it looks.
    face = surface_x(plain)
    on_surface = ridged(dict(center=(face, 0, 0), radius=0.32, strength=0.5))
    at_centre = surface_x(on_surface), face
    off_axis = surface_x(on_surface, y=0.15), surface_x(plain, y=0.15)
    print(f"  through the scale's centre: {at_centre[0]:.3f} vs {at_centre[1]:.3f} "
          f"(unmoved — it is the fixed point)")
    print(f"  off the axis:               {off_axis[0]:.3f} vs {off_axis[1]:.3f} "
          f"(moved)")
    if abs(at_centre[0] - at_centre[1]) > 0.01:
        raise SystemExit("the centre of the scale is supposed to be its fixed point")
    if off_axis[0] <= off_axis[1]:
        raise SystemExit("the deformer did nothing off the axis either")

    R.contact_sheet(
        [R.render_array(plain, eye=EYE, target=TARGET, width=310, height=290),
         R.render_array(on_surface, eye=EYE, target=TARGET, width=310, height=290)],
        "23_magnify_fixed_point.png", columns=2,
        caption="before, and magnified about a point on the right face — the point "
                "itself does not move, the form around it swells")

    # --- support is finite ----------------------------------------------------
    # Item influence bounds and brick culling both trust this.
    rng = np.random.default_rng(11)
    probes = rng.uniform(-1.6, 1.6, size=(6000, 3)).astype(np.float32)
    centre = np.array([face, 0.0, 0.0], np.float32)   # the deformer's own centre
    outside = np.linalg.norm(probes - centre, axis=1) > 0.32
    delta = np.abs(on_surface.eval(probes[outside]) - plain.eval(probes[outside]))
    print(f"  beyond the radius, the field differs by at most {delta.max():.2e}")
    if delta.max() > 1e-5:
        raise SystemExit("the deformer's support is not finite")

    # --- what scaling space costs --------------------------------------------
    print("  scaling space is not distance preserving, and the marcher pays:")
    for strength in (0.0, 0.25, 0.5, 0.8):
        doc = ridged(dict(center=(0, 0, 0), radius=0.95, strength=strength))
        print(f"    strength {strength:<5} -> step scale {doc.safe_step_scale():.3f}")
    scales = [ridged(dict(center=(0, 0, 0), radius=0.95, strength=s)).safe_step_scale()
              for s in (0.0, 0.25, 0.5, 0.8)]
    if not all(a >= b for a, b in zip(scales, scales[1:])):
        raise SystemExit("a stronger scale stopped costing more")

    # And the consequence: a ray still lands on the deformed surface.
    doc = ridged(dict(center=(0, 0, 0), radius=0.95, strength=0.5))
    result = doc.raycast_many(np.array([[3.0, 0, 0, -1, 0, 0]], np.float32))
    hit = bool(result["hit"][0])
    where = float(result["position"][0][0]) if hit else float("nan")
    print(f"  a ray still lands on the swelled surface: {hit} (x = {where:.3f})")
    if not hit or abs(where - surface_x(doc)) > 0.02:
        raise SystemExit("the raymarcher no longer finds the deformed surface")

    # --- voxels agree on what the verb means ---------------------------------
    grid = clay.VoxelGrid(0.05)
    colour = grid.palette_add("#b0784a")
    r = 7
    cells = [(x, y, z) for x in range(-r, r + 1) for y in range(-r, r + 1)
             for z in range(-r, r + 1) if x * x + y * y + z * z <= r * r]
    grid.set_many(np.array(cells, np.int32), colour)

    def mean_radius(g, at=(r, 0, 0)):
        got = [(x, y, z) for x in range(at[0] - 5, at[0] + 6)
               for y in range(at[1] - 5, at[1] + 6) for z in range(at[2] - 5, at[2] + 6)
               if g.get((x, y, z)) != 0]
        d = np.linalg.norm(np.array(got, float) - np.array(at, float), axis=1)
        return float(d.mean())

    before = mean_radius(grid)
    pinched = clay.VoxelGrid(0.05); pinched.palette_add("#b0784a")
    pinched.set_many(np.array(cells, np.int32), colour)
    pinched.sculpt_pinch((r, 0, 0), size=3)
    magnified = clay.VoxelGrid(0.05); magnified.palette_add("#b0784a")
    magnified.set_many(np.array(cells, np.int32), colour)
    magnified.sculpt_magnify((r, 0, 0), size=3)
    print(f"  voxels, mean radius from the brush: {mean_radius(pinched):.3f} pinched, "
          f"{before:.3f} start, {mean_radius(magnified):.3f} magnified")
    if not (mean_radius(pinched) < before < mean_radius(magnified)):
        raise SystemExit("the voxel verbs are not each other's inverse")

    R.export_model(ridged(dict(center=(0, 0, 0), radius=0.95, strength=0.45)),
                   "23_magnify.ply", resolution=72, decimate=0.08)


if __name__ == "__main__":
    main()
