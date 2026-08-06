"""The cut tool: a shape drawn over the model, cutting through it.

This is the practitioners' "90% tool" — ZBrush's Trim Rect / Trim Circle /
Trim Lasso, 3DCoat's Cut Off. Every ingredient already existed here: extruded
profiles, circles and boxes and arbitrary polygons, subtract and intersect,
rounding for bevelled walls. What did not exist was the step that turns *a
shape somebody drew* into that item, and leaving it to each caller means each
one answers "how deep" and "which side" differently.

Two decisions are worth reading before the code:

**The cut is a prism, not a frustum.** A shape drawn on screen under a
perspective camera sweeps a converging wedge, so a "correct" cut would
converge. It should not. A converging cut has a face that is not flat and a
result that depends on where the camera was standing — draw the same rectangle
from two positions and get two different solids. A trim is a straight cut. The
example asserts this: the same cut from a frame ten times further away gives
the identical solid.

**No camera enters the engine.** You give the *frame* the shape was drawn on,
which a viewport already has because it needed one to draw the overlay, and the
shape in world units on that frame. Not pixels, not normalized device
coordinates — the engine has no viewport and should not learn about one.

And one thing that is deliberately not a parameter: which side survives is the
**op** you place the cut with. Subtract removes what the shape covers,
intersect keeps only that. 3DCoat's "Shift = keep-outer" modifier is exactly
this choice, so a separate flag would be a second way to say one thing.
"""

import numpy as np

import pyclay as clay

import _render as R

FRONT = dict(origin=(0, 0, -4), right=(1, 0, 0), up=(0, 1, 0), forward=(0, 0, 1))


def block(size=1.8, color="#9aa4b0"):
    doc = clay.Document()
    layer = doc.add_sdf_layer("block")
    layer.add(clay.Box(size=(size, size, size)), color=color)
    return doc, layer


def main():
    R.banner("14 cut — a drawn shape, cut through the model")

    # --- the four shapes -----------------------------------------------------
    lasso = np.array([[-0.55, 0.0, 0, 0], [0.0, 0.55, 0, 0],
                      [0.55, 0.0, 0, 0], [0.0, -0.55, 0, 0]], np.float32)
    shapes = [
        ("rect", clay.CutShape.rect(0.45, 0.3)),
        ("circle", clay.CutShape.circle(0.45)),
        ("polygon", clay.CutShape.polygon(
            np.array([[-0.6, -0.4], [0.6, -0.4], [0.0, 0.6]], np.float32))),
        ("spline lasso", clay.CutShape.curve(lasso, types="spline", tolerance=0.005)),
    ]

    tiles = []
    for name, shape in shapes:
        doc, layer = block()
        layer.add(clay.Cut(shape=shape, region=doc, **FRONT), op=clay.Op.SUBTRACT)
        removed = int((doc.eval(_grid()) > 0).sum())
        print(f"  {name:13s} removed {removed:5d} of the sampled interior points")
        tiles.append(R.render_array(doc, eye=(1.6, 1.9, -3.0), target=(0, 0, 0),
                                    width=205, height=195))
    R.contact_sheet(tiles, "14_cut_shapes.png", columns=4,
                    caption="rect, circle, polygon and a spline lasso, cut from the front")

    # --- the cut is a prism --------------------------------------------------
    probes = np.random.default_rng(2).uniform(-1.1, 1.1, size=(4096, 3)).astype(np.float32)
    solids = []
    for z in (-4.0, -40.0):
        doc, layer = block()
        frame = dict(FRONT, origin=(0, 0, z))
        layer.add(clay.Cut(shape=clay.CutShape.rect(0.45, 0.3), region=doc, **frame),
                  op=clay.Op.SUBTRACT)
        solids.append(doc.eval(probes) < 0)
    same = bool(np.array_equal(solids[0], solids[1]))
    print(f"  the same cut from 10x further away gives the identical solid: {same}")
    if not same:
        raise SystemExit("the cut converges with distance — it is not a prism")

    # --- which side survives is the op ---------------------------------------
    tiles = []
    for name, op in (("subtract", clay.Op.SUBTRACT), ("intersect", clay.Op.INTERSECT)):
        doc, layer = block()
        layer.add(clay.Cut(shape=clay.CutShape.circle(0.5), region=doc, **FRONT), op=op)
        tiles.append(R.render_array(doc, eye=(1.6, 1.9, -3.0), target=(0, 0, 0),
                                    width=230, height=210))
        print(f"  {name:9s} -> {'a hole' if op == clay.Op.SUBTRACT else 'only the plug'}")
    R.contact_sheet(tiles, "14_cut_sides.png", columns=2,
                    caption="the same cut placed with SUBTRACT and with INTERSECT")

    # A point inside the block survives exactly one of the two.
    interior = _grid()
    a, b = [], []
    for op, out in ((clay.Op.SUBTRACT, a), (clay.Op.INTERSECT, b)):
        doc, layer = block()
        layer.add(clay.Cut(shape=clay.CutShape.circle(0.5), region=doc, **FRONT), op=op)
        out.append(doc.eval(interior) < 0)
    overlap = int((a[0] & b[0]).sum())
    print(f"  no interior point survives both: {overlap == 0}")
    if overlap != 0:
        raise SystemExit("subtract and intersect are not complementary")

    # --- depth and bevel -----------------------------------------------------
    tiles = []
    for name, kwargs in (("through", {}),
                         ("partial", dict(near=0.0, far=4.0)),
                         ("bevelled", dict(rounding=0.12))):
        doc, layer = block()
        layer.add(clay.Cut(shape=clay.CutShape.rect(0.35, 0.35), region=doc, **FRONT,
                           **kwargs), op=clay.Op.SUBTRACT)
        far_face = float(doc.eval(np.array([[0, 0, 0.85]], np.float32))[0])
        print(f"  {name:9s} -> the far face is "
              f"{'open' if far_face > 0 else 'still solid'}")
        tiles.append(R.render_array(doc, eye=(1.9, 1.7, -2.8), target=(0, 0, 0),
                                    width=215, height=200))
    R.contact_sheet(tiles, "14_cut_depth.png", columns=3,
                    caption="a cut through, a deliberate partial cut, and bevelled walls")

    # --- an angled frame -----------------------------------------------------
    # The same call from a different plane: nothing about the cut knows which
    # way "the camera" is pointing, only what frame it was handed.
    doc, layer = block()
    layer.add(clay.Cut(origin=(0, 3, 0), right=(1, 0, 0), up=(0, 0, 1), forward=(0, -1, 0),
                       shape=clay.CutShape.circle(0.35), region=doc), op=clay.Op.SUBTRACT)
    top = float(doc.eval(np.array([[0, 0.85, 0]], np.float32))[0])
    print(f"  a frame looking down cuts top to bottom instead: {top > 0}")
    if top <= 0:
        raise SystemExit("the cut ignored its frame's sweep direction")
    R.render(doc, "14_cut_from_above.png", eye=(2.0, 2.4, -2.4), target=(0, 0, 0),
             caption="the same call, a frame looking down")

    R.export_model(doc, "14_cut.ply", resolution=56, decimate=0.08)


def _grid(n=24, half=0.85):
    """Points spread through the block's interior."""
    a = np.linspace(-half, half, n)
    x, y, z = np.meshgrid(a, a, a, indexing="ij")
    return np.stack([x.ravel(), y.ravel(), z.ravel()], axis=1).astype(np.float32)


if __name__ == "__main__":
    main()
