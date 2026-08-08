"""Move Topological — a drag that knows what is connected to what.

`grab` weights its falloff by `|p - centre| / radius`: Euclidean distance
through SPACE. Two fingers 0.32 apart are 0.32 apart to it, however far apart
they are along the form — so a drag on one pulls the other, which is exactly
what a sculptor does not want.

ZBrush's Move Topological weights the same falloff by distance along the
MATERIAL. Down one finger, across the palm and up the other is about 1.5, so at
a radius of 0.5 the neighbour is out of reach and stays where it is. The first
section measures both against the same drag.

Three things are worth reading before looking.

**The radius means something different.** It is a distance of travel across the
surface, not a straight line, so it cannot step over a gap however narrow the
gap is. Raise it past the path through the joining body and the neighbour does
come into range — which is the second section, and what shows the weight is a
distance rather than a mask.

**It bakes**, for the reason relax and flatten do: the weight is a geodesic
field solved on a grid, not a closed form, and a deformer is a kernel function
over a handful of floats. Putting a solved grid in the tape would mean a
deformer that reads out-of-line data, which no deformer does and which all four
backends would have to grow. So this is the expensive one — reach for
`Layer.move_surface` when the form has no parts close in space and far along the
surface.

**Two things about the solve were found by measuring, not by reasoning.** The
weight has to extend into free space by at least the displacement, or the
material never arrives and the grabbed part is torn off at its own boundary — a
fixed three-cell shell against a drag of 0.25 left a finger 0.044 wide where it
started at 0.204. And that shell has to grow only ALONG the drag: grown in every
direction it put a weight in the gap between the fingers, where sampling reached
across into the other finger and deposited a slab of it in mid-air.
"""

import numpy as np

import pyclay as clay

import _render as R


def hand(colour="#b0784a"):
    """Two fingers, joined only through a palm below them."""
    doc = clay.Document()
    layer = doc.add_sdf_layer("l")
    layer.add(clay.Sphere(r=0.40, position=(0, -0.38, 0)), color=colour)
    for x in (-0.26, 0.26):
        layer.add(clay.Stroke(points=np.array([[x, -0.15, 0, 0.12],
                                               [x, 0.58, 0, 0.10]], np.float32)),
                  color=colour, blend=clay.Smooth(0.03))
    return doc, layer


def runs(doc, y):
    """The spans of material along X at a height — one per finger."""
    xs = np.arange(-1.2, 1.2, 0.002, dtype=np.float32)
    pts = np.stack([xs, np.full_like(xs, y), np.zeros_like(xs)], axis=1)
    inside = np.nonzero(doc.eval(pts) <= 0)[0]
    if not len(inside):
        return []
    out, start = [], inside[0]
    for a, b in zip(inside, inside[1:]):
        if b != a + 1:
            out.append((float(xs[start]), float(xs[a])))
            start = b
    out.append((float(xs[start]), float(xs[inside[-1]])))
    return out


def show(spans):
    return "  ".join(f"[{a:+.3f},{b:+.3f}] w={b - a:.3f}" for a, b in spans)


def topological(source, radius, displacement=(-0.25, 0, 0), cell=0.015):
    volume = clay.Volume.moved_topologically_from(
        source, anchor=(-0.26, 0.45, 0), displacement=displacement, radius=radius, cell=cell)
    doc = clay.Document()
    doc.add_sdf_layer("t").add(volume, color="#b0784a")
    return doc, volume


def main():
    R.banner("31 move topological — a drag that knows what is connected to what")

    EYE, TARGET = (1.6, 1.0, 2.6), (0, 0.1, 0)
    base, _ = hand()
    DRAG = (-0.25, 0, 0)

    # --- the same drag, two ways ---------------------------------------------
    euclid, layer = hand()
    layer.move_surface((-0.26, 0.45, 0), DRAG, radius=0.50)
    topo, volume = topological(base, 0.50)

    print("  two fingers 0.32 apart, joined only through the palm; drag the left one:")
    print(f"    {'before':<20}{show(runs(base, 0.45))}")
    print(f"    {'euclidean r=0.50':<20}{show(runs(euclid, 0.45))}")
    print(f"    {'topological r=0.50':<20}{show(runs(topo, 0.45))}")

    before = runs(base, 0.45)
    after_e = runs(euclid, 0.45)
    after_t = runs(topo, 0.45)
    if len(before) != 2 or len(after_t) != 2:
        raise SystemExit(f"the fingers stopped being two separate spans: {before} {after_t}")
    # The far finger must be untouched by the topological drag and moved by the
    # Euclidean one. That difference IS the brush.
    if abs(after_t[1][0] - before[1][0]) > 0.01:
        raise SystemExit("the topological drag reached the far finger")
    if abs(after_e[1][0] - before[1][0]) < 0.05:
        raise SystemExit("the euclidean drag stopped reaching the far finger")
    # ...and the grabbed finger moves, keeping its width.
    if after_t[0][0] > before[0][0] - 0.1:
        raise SystemExit("the topological drag did not move the finger it was aimed at")
    if abs((after_t[0][1] - after_t[0][0]) - (before[0][1] - before[0][0])) > 0.05:
        raise SystemExit("the grabbed finger changed width — it should translate")

    R.contact_sheet(
        [R.render_array(base, eye=EYE, target=TARGET, width=205, height=195),
         R.render_array(euclid, eye=EYE, target=TARGET, width=205, height=195),
         R.render_array(topo, eye=EYE, target=TARGET, width=205, height=195)],
        "31_move_topological.png", columns=3,
        caption="before, the same drag weighted by distance through SPACE, and weighted by "
                "distance along the MATERIAL — only the second leaves the far finger alone")

    # --- the radius is a distance along the surface ---------------------------
    # Raise it past the path through the palm and the far finger does come into
    # range, which is what shows this is a distance and not a mask.
    print("  raising the geodesic radius until the far finger is within reach:")
    print(f"    {'radius':>8}{'far finger near edge':>23}")
    print(f"    {'(before)':>8}{before[1][0]:>23.3f}")
    edges = []
    for radius in (0.5, 1.0, 1.6, 2.2):
        doc, _ = topological(base, radius, cell=0.02)
        spans = runs(doc, 0.45)
        edge = spans[-1][0] if spans else float("nan")
        edges.append(edge)
        print(f"    {radius:>8}{edge:>23.3f}")
    if not (abs(edges[0] - before[1][0]) < 0.01 and edges[-1] < before[1][0] - 0.02):
        raise SystemExit(f"the reach stopped running along the material: {edges}")

    # --- what it costs --------------------------------------------------------
    print(f"  it BAKES: {volume.brick_count} bricks, {volume.megabytes:.2f} MB, "
          f"declared lipschitz {volume.sample_lipschitz:.2f}, step scale "
          f"{topo.safe_step_scale():.3f}")
    print("  a Euclidean move stays an ordinary edit and does not bake — reach for this "
          "one only when parts are close in space and far along the surface")

    R.export_model(topo, "31_move_topological.ply", resolution=72, decimate=0.08)


if __name__ == "__main__":
    main()
