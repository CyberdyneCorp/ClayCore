"""Relief and incise — an item used as a region, not as a shape.

Every other combine mode treats the item as geometry: add unions it, subtract
carves it away, engrave and emboss cut and raise along where its surface crosses
the accumulated one. These two do something categorically different. The item's
field is read as a **region** — a weight saying *where* — and what happens there
is that the surface already accumulated moves along its own normal. The item
contributes no geometry of its own at all.

That is what ZBrush's Standard and ClayBuildup do when they build a surface up,
and what Crease and DamStandard do when they cut a line into one. You do not
union a sphere onto a face to raise a bump; you say "raise the surface here, by
this much, tapering out over this distance."

Three things are worth reading before looking.

**The item alone is not a surface.** The second section renders a relief item
with nothing under it and finds no geometry anywhere, then puts a ball under it
and finds the ball, displaced. If this ever renders a sphere, the op has
silently become an ordinary union.

**The rounding does double duty, and this is the one surprise.** It is the
falloff width, and it also rounds the region's own field — exactly as it does
for groove and tongue, where the channel is centered on the *rounded* surface.
So the reach is radius + rounding + falloff, not radius + falloff. This caught
the finite-support test before it caught anything else, and the third section
measures the real number rather than assuming it.

**Amplitude over falloff width is one number seen twice.** It is what makes the
rim hard — raise the surface further than the taper is wide and the shoulder
becomes a ledge — and it is also exactly the slope the op adds, because offsetting
a distance is not distance preserving. So the picture and the safe step scale are
the same quantity: the fourth section renders the ratio and prints it.
"""

import numpy as np

import pyclay as clay

import _render as R


def ball_with(op=None, amplitude=0.12, width=0.14, at=(0, 0.7, 0), region=None):
    """A ball, optionally with a relief region sitting on top of it."""
    doc = clay.Document()
    layer = doc.add_sdf_layer("l")
    layer.add(clay.Sphere(r=0.7), color="#b0784a")
    if op is not None:
        layer.add(region if region is not None else clay.Sphere(r=0.24, position=at),
                  op=op, blend=clay.Smooth(amplitude), rounding=width)
    return doc


def top(doc):
    """Where the surface sits on the +Y axis."""
    ys = np.arange(0.2, 1.4, 0.001, dtype=np.float32)
    pts = np.stack([np.zeros_like(ys), ys, np.zeros_like(ys)], axis=1)
    out = np.nonzero(doc.eval(pts) > 0.0)[0]
    return float(ys[out[0]]) if len(out) else float("nan")


def main():
    R.banner("25 relief and incise — an item used as a region, not as a shape")

    EYE, TARGET = (2.2, 1.4, 2.3), (0, 0.2, 0)

    # --- the two directions ---------------------------------------------------
    # One kernel branch with the sign taken from the mode, which is what keeps
    # them each other's exact inverse as either changes.
    base = top(ball_with())
    print(f"  the plain ball's surface is at y = {base:.3f}")
    tiles, labels = [], []
    for op, amp in ((clay.Op.INCISE, 0.16), (clay.Op.INCISE, 0.08), (None, 0.0),
                    (clay.Op.RELIEF, 0.08), (clay.Op.RELIEF, 0.16)):
        doc = ball_with(op, amplitude=amp)
        y = top(doc)
        name = "plain" if op is None else ("relief" if op is clay.Op.RELIEF else "incise")
        print(f"    {name:<6} amplitude {amp:<5} -> surface at y = {y:.3f} "
              f"({y - base:+.3f})")
        tiles.append(R.render_array(doc, eye=EYE, target=TARGET, width=180, height=180))
        labels.append(f"{name} {amp}" if op else "plain")
    R.contact_sheet(tiles, "25_relief_sweep.png", columns=5, caption=", ".join(labels))

    # Offsetting a distance field moves its isosurface along the field's own
    # gradient — the surface normal — by exactly the offset. Not approximately.
    for amp in (0.06, 0.12, 0.2):
        up = top(ball_with(clay.Op.RELIEF, amplitude=amp)) - base
        down = base - top(ball_with(clay.Op.INCISE, amplitude=amp))
        if abs(up - amp) > 0.02 or abs(down - amp) > 0.02:
            raise SystemExit(f"amplitude {amp} moved the surface by {up:+.3f}/{down:+.3f}")
        if abs(up - down) > 0.01:
            raise SystemExit("the two directions stopped being each other's inverse")

    # --- the item is a region, not geometry -----------------------------------
    # The whole distinction. A relief item alone has nothing to displace, so it
    # is not a surface. If this ever renders a sphere, the op has quietly become
    # an ordinary union.
    solo = clay.Document()
    solo.add_sdf_layer("l").add(clay.Sphere(r=0.24, position=(0, 0.7, 0)),
                                op=clay.Op.RELIEF, blend=clay.Smooth(0.16),
                                rounding=0.14, color="#b0784a")
    rng = np.random.default_rng(4)
    inside = int((solo.eval(rng.uniform(-1.2, 1.2, size=(6000, 3)).astype(np.float32))
                  < 0).sum())
    print(f"  a relief item with nothing under it: {inside} of 6000 probes inside it")
    if inside != 0:
        raise SystemExit("the region became geometry — this is an ordinary union now")

    R.contact_sheet(
        [R.render_array(solo, eye=EYE, target=TARGET, width=310, height=290),
         R.render_array(ball_with(clay.Op.RELIEF, amplitude=0.16),
                        eye=EYE, target=TARGET, width=310, height=290)],
        "25_relief_is_a_region.png", columns=2,
        caption="the same relief item alone, and over a ball — it is not geometry, "
                "it says where the surface already there should move")

    # --- the rounding does double duty ----------------------------------------
    # The surprise. It is the falloff width AND it rounds the region's own field,
    # exactly as it does for groove and tongue. So reach = radius + rounding +
    # falloff. Measured, not assumed.
    radius, width = 0.24, 0.14
    plain = ball_with()
    relieved = ball_with(clay.Op.RELIEF, amplitude=0.12, width=width)
    probes = rng.uniform(-1.5, 1.6, size=(20000, 3)).astype(np.float32)
    reach = np.linalg.norm(probes - np.array([0, 0.7, 0], np.float32), axis=1)
    delta = np.abs(relieved.eval(probes) - plain.eval(probes))
    touched = reach[delta > 1e-5]
    measured = float(touched.max()) if len(touched) else 0.0
    print(f"  the region reaches {measured:.3f} from its centre; radius {radius} + "
          f"rounding {width} + falloff {width} = {radius + 2 * width:.2f}")
    if measured > radius + 2 * width + 1e-3:
        raise SystemExit("the support is wider than the bounds are dilated by")
    if measured < radius + width:
        raise SystemExit("the rounding is no longer growing the region's own field")

    # --- any primitive can be the region --------------------------------------
    # A sphere for a round brush, a box for a chisel. The region is whatever
    # primitive the item happens to carry.
    R.contact_sheet(
        [R.render_array(ball_with(clay.Op.RELIEF, amplitude=0.14, width=0.2,
                                  region=clay.Box(size=(0.3, 0.3, 0.1),
                                                  position=(0, 0.7, 0))),
                        eye=EYE, target=TARGET, width=310, height=290),
         R.render_array(ball_with(clay.Op.INCISE, amplitude=0.14, width=0.06,
                                  region=clay.Box(size=(0.02, 0.02, 0.5),
                                                  position=(0, 0.7, 0))),
                        eye=EYE, target=TARGET, width=310, height=290)],
        "25_relief_regions.png", columns=2,
        caption="a box region raising a pad, and a thin one incising a crease — "
                "Standard and DamStandard are the same op with different regions")

    for r in (clay.Sphere(r=0.3, position=(0, 0.7, 0)),
              clay.Box(size=(0.3, 0.3, 0.1), position=(0, 0.7, 0))):
        if top(ball_with(clay.Op.RELIEF, amplitude=0.14, width=0.2, region=r)) <= base:
            raise SystemExit("a box region stopped displacing the surface")

    # --- one ratio sets both the look and the cost ----------------------------
    # Amplitude over falloff width is what makes the rim hard: raise the surface
    # further than the taper is wide and the shoulder becomes a ledge. It is also
    # exactly the slope the op adds, so it is the number the tape carries and the
    # number the raymarcher pays for. The picture and the step scale are the same
    # quantity seen twice.
    print("  amplitude / falloff sets the rim, and it is the slope the tape carries:")
    tiles, labels = [], []
    for w in (0.06, 0.14, 0.3):
        doc = ball_with(clay.Op.RELIEF, amplitude=0.14, width=w)
        print(f"    amplitude 0.14 falloff {w:<5} (ratio {0.14 / w:.1f}) -> step scale "
              f"{doc.safe_step_scale():.3f}")
        tiles.append(R.render_array(doc, eye=EYE, target=TARGET, width=205, height=195))
        labels.append(f"falloff {w}")
    R.contact_sheet(tiles, "25_relief_falloff.png", columns=3,
                    caption=", ".join(labels) +
                            " at one amplitude — a narrow taper is a ledge, a wide "
                            "one is a swell, and the narrow one is what costs the marcher")

    scales = [ball_with().safe_step_scale()] + [
        ball_with(clay.Op.RELIEF, amplitude=a, width=w).safe_step_scale()
        for a, w in ((0.06, 0.3), (0.14, 0.3), (0.14, 0.06))]
    if not all(a >= b for a, b in zip(scales, scales[1:])):
        raise SystemExit("a deeper relief or a narrower falloff stopped costing more")

    # And the consequence: a ray still lands on the displaced surface.
    doc = ball_with(clay.Op.RELIEF, amplitude=0.16)
    result = doc.raycast_many(np.array([[0, 3.0, 0, 0, -1, 0]], np.float32))
    hit = bool(result["hit"][0])
    where = float(result["position"][0][1]) if hit else float("nan")
    print(f"  a ray still lands on the raised surface: {hit} (y = {where:.3f})")
    if not hit or abs(where - top(doc)) > 0.02:
        raise SystemExit("the raymarcher no longer finds the displaced surface")

    # A crease, before and after — seen from above, where a groove reads as one.
    # `size` is the FULL extent, so this bar spans x in [-0.45, 0.45].
    bar = clay.Box(size=(0.9, 0.04, 0.04), position=(0, 0.66, 0))
    creased = ball_with(clay.Op.INCISE, amplitude=0.14, width=0.05, region=bar)
    R.contact_sheet(
        [R.render_array(ball_with(), eye=(0.9, 2.3, 0.9), target=(0, 0.5, 0),
                        width=300, height=290),
         R.render_array(creased, eye=(0.9, 2.3, 0.9), target=(0, 0.5, 0),
                        width=300, height=290)],
        "25_relief_crease.png", columns=2,
        caption="before, and a thin box region incising a crease across the top — "
                "no geometry was subtracted, the surface itself was pushed in")

    # And it really is pushed in, not raised: the eye is a poor judge of a groove
    # at a glancing angle, so measure the surface along the bar and off it.
    def surface_at(doc, x=0.0, z=0.0):
        ys = np.arange(0.2, 1.3, 0.001, dtype=np.float32)
        pts = np.stack([np.full_like(ys, x), ys, np.full_like(ys, z)], axis=1)
        out = np.nonzero(doc.eval(pts) > 0.0)[0]
        return float(ys[out[0]]) if len(out) else float("nan")

    plain = ball_with()
    on_bar = surface_at(creased) - surface_at(plain)
    clear_of_bar = surface_at(creased, z=0.3) - surface_at(plain, z=0.3)
    print(f"  on the crease the surface moves {on_bar:+.3f}; clear of it, "
          f"{clear_of_bar:+.3f}")
    if on_bar >= -0.1:
        raise SystemExit("incise raised the surface instead of cutting into it")
    if abs(clear_of_bar) > 1e-3:
        raise SystemExit("the crease is not staying inside its region")

    R.export_model(ball_with(clay.Op.RELIEF, amplitude=0.16), "25_relief.ply",
                   resolution=72, decimate=0.08)


if __name__ == "__main__":
    main()
