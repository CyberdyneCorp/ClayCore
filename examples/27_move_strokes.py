"""A sphere and three big Move strokes — and what a field does that a mesh does not.

The reference for this example is a ZBrush screenshot: a sphere with three large
Move strokes pulled out into broad tapered lobes. Reproducing it is the obvious
test of the Move brush, and the result is worth having as an example precisely
because **Move does not get there**, for a reason that is structural rather than
a tuning problem.

**A mesh stretches; a field moves what is already there.** ZBrush's Move drags a
region of *vertices* and the surface between them stretches, because a mesh is a
connected sheet. `grab` — the deformation under `move_surface` — translates a
region of *space*: the field is sampled at `p - w(|p - c|/r)·d`. Where the weight
is one the material is rigidly displaced, and where it falls to zero nothing
happens. Material can only appear where material already was, so a large pull
does not draw a horn out of the surface. It buds a lump off it.

The first section measures that. Pulling harder does not help: the surface gains
+0.34 for a displacement of 1.1 and +0.31 for a displacement of 2.5, because the
reach is bounded by the falloff rather than by how far you drag.

**And a stroke is many drags, which compounds.** An artist pulls a lobe by
dragging repeatedly, walking the brush outward. Each drag is another grab on the
chain, each multiplies the declared Lipschitz, and the safe step scale decays
geometrically — about x0.615 per drag here. Six drags cost 18x, nine cost 79x,
and a three-stroke sculpt at that rate is not marchable at all. Coalescing
covers frames of ONE drag, where the centre and radius are fixed; a stroke moves
the centre, so it stacks by design.

**What does reproduce the reference is `snakehook`**, and the last section shows
it next to the Move attempt. It sweeps a tapered stroke item along the drag,
which ADDS material instead of displacing it — so it reaches as far as the drag
goes, and the field stays exact: the step scale is 1.0 with three lobes on it,
against 0.05 for the Move version.

The honest summary is that Move is the right verb for nudging a form and the
wrong one for growing one, and the engine has a separate verb for growing.
"""

import numpy as np

import pyclay as clay

import _render as R

# Three directions to pull in, roughly matching the reference's lobes.
DIRS = [(1.0, 0.5, 0.05), (-0.8, -0.25, 0.55), (0.1, -0.85, -0.5)]


def unit(v):
    v = np.array(v, np.float32)
    return v / np.linalg.norm(v)


def reach_along(doc, direction, hi=4.0):
    """How far the surface extends from the origin along a direction."""
    u = unit(direction)
    ts = np.arange(0.0, hi, 0.002, dtype=np.float32)
    pts = (ts[:, None] * u[None, :]).astype(np.float32)
    inside = np.nonzero(doc.eval(pts) <= 0)[0]
    return float(np.linalg.norm(pts[inside[-1]])) if len(inside) else float("nan")


def ball(colour="#b0463f"):
    doc = clay.Document()
    layer = doc.add_sdf_layer("l")
    layer.add(clay.Sphere(r=1.0), color=colour)
    return doc, layer


def moved(radius, displacement):
    """One big Move stroke per direction, anchored on the surface."""
    doc, layer = ball()
    for d in DIRS:
        u = unit(d)
        layer.move_surface(tuple(map(float, u)), tuple(map(float, u * displacement)),
                           radius=radius)
    return doc


def hooked(base_radius=0.55, reach=1.5, taper=0.9):
    """The same three gestures as snakehooks."""
    doc, layer = ball()
    for d in DIRS:
        u = unit(d)
        anchor = u * 1.0
        path = np.array([anchor + u * t for t in np.linspace(0.05, reach, 7)], np.float32)
        tendril = clay.snakehook(tuple(map(float, anchor)), tuple(map(float, -u)), path,
                                 base_radius=base_radius, tip_fraction=0.12,
                                 taper_curve=taper)
        layer.add(tendril, color="#b0463f", blend=clay.Smooth(0.35))
    return doc


def main():
    R.banner("27 move strokes — what a field does that a mesh does not")

    EYE, TARGET = (4.0, 2.1, 4.0), (0, 0, 0)

    # --- pulling harder does not pull further --------------------------------
    print("  three Move strokes on a unit sphere, pulled with a large area:")
    print(f"    {'radius':>7} {'displacement':>13} {'reach':>7} {'gain':>7} {'step scale':>11}")
    tiles, labels = [], []
    for radius, disp in ((1.1, 1.1), (0.8, 1.1), (0.5, 1.1), (0.5, 2.5)):
        doc = moved(radius, disp)
        r = reach_along(doc, DIRS[0])
        print(f"    {radius:>7} {disp:>13} {r:>7.3f} {r - 1.0:>+7.3f} "
              f"{doc.safe_step_scale():>11.4f}")
        tiles.append(R.render_array(doc, eye=EYE, target=TARGET, width=205, height=195))
        labels.append(f"r {radius}, d {disp}")
    R.contact_sheet(tiles, "27_move_buds.png", columns=4, caption=", ".join(labels) +
                    " — every one buds a lump rather than drawing a lobe out")

    gentle = reach_along(moved(0.5, 1.1), DIRS[0]) - 1.0
    hard = reach_along(moved(0.5, 2.5), DIRS[0]) - 1.0
    print(f"  more than doubling the drag adds {hard - gentle:+.3f} to the reach: the "
          f"falloff bounds it, not the drag")
    if hard > gentle * 1.5:
        raise SystemExit("the reach stopped being bounded by the falloff")

    # --- a stroke is many drags, and they compound ---------------------------
    # An artist walks the brush outward. Each drag is another grab on the chain
    # and each multiplies the declared Lipschitz, so the cost is geometric.
    print("  a stroke is many drags, and the safe step scale decays per drag:")
    doc, layer = ball()
    scales = [doc.safe_step_scale()]
    for i in range(9):
        layer.move_surface((1.0 + 0.25 * i, 0, 0), (0.25, 0, 0), radius=0.5)
        scales.append(doc.safe_step_scale())
    for n in (1, 3, 6, 9):
        print(f"    {n} drag(s): step scale {scales[n]:.4f}  ({1.0 / scales[n]:.0f}x the "
              f"marching cost)")
    ratios = [b / a for a, b in zip(scales[1:], scales[2:])]
    print(f"  each drag multiplies it by about {np.mean(ratios):.3f} — geometric, so a "
          f"long stroke is not marchable")
    if not all(b < a for a, b in zip(scales, scales[1:])):
        raise SystemExit("stacking drags stopped costing the marcher")
    if scales[9] > 0.05:
        raise SystemExit("nine drags no longer collapse the step scale — recheck this claim")

    # --- what does reproduce the reference ------------------------------------
    move_version = moved(0.8, 1.1)
    hook_version = hooked()
    print("  the same three gestures, as Move and as snakehook:")
    for name, doc in (("move", move_version), ("snakehook", hook_version)):
        reaches = " ".join(f"{reach_along(doc, d):.2f}" for d in DIRS)
        print(f"    {name:<10} reach {reaches}   step scale {doc.safe_step_scale():.3f}")

    # Snakehook ADDS material along the drag, so it reaches as far as the drag
    # goes — and the field stays exact, which the step scale shows.
    if reach_along(hook_version, DIRS[0]) <= reach_along(move_version, DIRS[0]):
        raise SystemExit("snakehook stopped reaching further than move")
    if hook_version.safe_step_scale() < 0.99:
        raise SystemExit("snakehook stopped being exact")

    R.contact_sheet(
        [R.render_array(move_version, eye=(4.6, 2.4, 4.6), target=TARGET,
                        width=310, height=290),
         R.render_array(hook_version, eye=(4.6, 2.4, 4.6), target=TARGET,
                        width=310, height=290)],
        "27_move_vs_snakehook.png", columns=2,
        caption="three Move strokes, and the same three gestures as snakehooks — "
                "Move displaces what is there, snakehook grows what is not")

    R.export_model(hook_version, "27_move_strokes.ply", resolution=80, decimate=0.08)


if __name__ == "__main__":
    main()
