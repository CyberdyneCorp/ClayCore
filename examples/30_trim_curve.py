"""Trim Curve — an open stroke that takes one side of the form.

ZBrush's Trim Curve: draw a stroke across the model and everything to one side
of it goes. The cut tool already did the hard parts — a shape drawn on a frame
resolves into an ordinary extruded item, and a control-point curve flattens
through the same tessellator a curve item uses — but it did them for a **lasso**,
and a lasso is not a trim.

**The difference is one word, and it is not a flag.** `CutShape.curve`
tessellates CLOSED: it joins the stroke's endpoints, so the outline is a loop and
the cut is its inside. A trim stroke's endpoints must stay apart, because what
closes the outline is the frame's own boundary on the side being discarded. Given
the same control points the two produce different polygons and different fields,
which the second section measures — so they are separate constructors rather than
one with a flag.

Two things are worth reading before looking.

**Which side goes is the OP, not the shape.** `side` names the half the outline
*covers*; placing it with `SUBTRACT` removes that half and with `INTERSECT` keeps
only it. That is the rule the cut tool already follows for keep-inner against
keep-outer, and a separate flag would be a second way to say one thing.

**A trim is a prism, not a frustum.** The sweep is parallel and the caller passes
the frame, because no camera enters the engine: a stroke drawn under a
perspective camera would sweep a converging wedge, and cutting with one gives a
face that is not flat and a solid that depends on where you were standing.
"""

import numpy as np

import pyclay as clay

import _render as R

# An open stroke drawn across the form, in the frame's own 2D coordinates.
STROKE = np.array([[x, 0.22 * np.sin(x * 2.6), 0.0, 0.0]
                   for x in np.linspace(-1.3, 1.3, 11)], np.float32)


def form(colour="#b0784a"):
    doc = clay.Document()
    layer = doc.add_sdf_layer("l")
    layer.add(clay.RoundBox(size=(1.0, 0.9, 0.75), r=0.22), color=colour)
    return doc, layer


def trimmed(side, op=None, rounding=0.0):
    doc, layer = form()
    shape = clay.CutShape.trim(STROKE, side=side, extent=(3.0, 3.0, 0.0))
    layer.add(clay.Cut(origin=(0, 0, -3.0), right=(1, 0, 0), up=(0, 1, 0),
                       forward=(0, 0, 1), shape=shape, region=doc, rounding=rounding),
              op=op if op is not None else clay.Op.SUBTRACT)
    return doc


def at(doc, y, x=0.0, z=0.0):
    return float(doc.eval(np.array([[x, y, z]], np.float32))[0])


def main():
    R.banner("30 trim curve — an open stroke that takes one side")

    EYE, TARGET = (2.2, 1.5, 2.6), (0, 0, 0)

    # --- the trim, and which side it takes ------------------------------------
    below = trimmed("below")
    above = trimmed("above")
    plain, _ = form()
    print("  the same stroke, covering one side or the other, placed with SUBTRACT:")
    print(f"    {'shape covers':>14}{'below the stroke':>19}{'above it':>12}")
    for name, doc in (("(untouched)", plain), ("below", below), ("above", above)):
        print(f"    {name:>14}{at(doc, -0.30):>19.3f}{at(doc, 0.30):>12.3f}")

    # Complementary: what one removes the other keeps.
    if not (at(below, -0.30) > 0 and at(below, 0.30) < 0):
        raise SystemExit("trimming below did not remove the lower half")
    if not (at(above, -0.30) < 0 and at(above, 0.30) > 0):
        raise SystemExit("trimming above did not remove the upper half")

    R.contact_sheet(
        [R.render_array(plain, eye=EYE, target=TARGET, width=205, height=195),
         R.render_array(below, eye=EYE, target=TARGET, width=205, height=195),
         R.render_array(above, eye=EYE, target=TARGET, width=205, height=195),
         R.render_array(trimmed("below", clay.Op.INTERSECT), eye=EYE, target=TARGET,
                        width=205, height=195)],
        "30_trim_curve.png", columns=4,
        caption="untouched, cover-below + SUBTRACT, cover-above + SUBTRACT, cover-below "
                "+ INTERSECT — the last two match, which is the shape/op split as an identity")

    # Covering ABOVE and subtracting is the same solid as covering BELOW and
    # intersecting — which is the shape/op split stated as an identity, and the
    # reason a "which side survives" flag would be redundant.
    keep_below = trimmed("below", clay.Op.INTERSECT)
    rng0 = np.random.default_rng(11)
    cloud = rng0.uniform(-1.2, 1.2, size=(5000, 3)).astype(np.float32)
    # Compared as SOLIDS, not as fields: subtract is max(a, -b) and intersect is
    # max(a, b), which agree on the sign and disagree on the distance outside the
    # surface. The claim is about which material survives.
    same = float(np.mean((above.eval(cloud) < 0) == (keep_below.eval(cloud) < 0)))
    print(f"  covering above + SUBTRACT and covering below + INTERSECT keep the same "
          f"material at {same * 100:.1f}% of probes")
    if same < 0.999:
        raise SystemExit("the shape/op split stopped being an identity")

    # --- a lasso of the same points is a different cut ------------------------
    # The reason these are separate constructors: closing the stroke joins its
    # endpoints and cuts a sliver between them rather than dividing the frame.
    lasso_doc, lasso_layer = form()
    lasso = clay.CutShape.curve(STROKE)
    lasso_layer.add(clay.Cut(origin=(0, 0, -3.0), right=(1, 0, 0), up=(0, 1, 0),
                             forward=(0, 0, 1), shape=lasso, region=lasso_doc),
                    op=clay.Op.SUBTRACT)
    rng = np.random.default_rng(5)
    probes = rng.uniform(-1.1, 1.1, size=(6000, 3)).astype(np.float32)
    differ = float(np.abs(below.eval(probes) - lasso_doc.eval(probes)).max())
    kept_below = int((lasso_doc.eval(probes) < 0).sum())
    print(f"  the same points as a closed lasso differ by up to {differ:.3f}; it leaves "
          f"{kept_below} of 6000 probes inside against {int((below.eval(probes) < 0).sum())} "
          f"for the trim")
    if differ < 0.05:
        raise SystemExit("a closed lasso stopped differing from an open trim")

    # --- rounding bevels the trimmed wall -------------------------------------
    print("  rounding bevels the cut wall, as it does for every cut:")
    for rounding in (0.0, 0.06, 0.14):
        doc = trimmed("below", rounding=rounding)
        print(f"    rounding {rounding:<5} -> the field just above the cut reads "
              f"{at(doc, 0.06):+.4f}")
    R.contact_sheet(
        [R.render_array(trimmed("below", rounding=r), eye=(1.6, 1.1, 2.2), target=(0, 0.1, 0),
                        width=205, height=195) for r in (0.0, 0.06, 0.14)],
        "30_trim_rounding.png", columns=3,
        caption="rounding 0, 0.06, 0.14 — a trim is an ordinary extruded item, so it "
                "bevels like every other cut")

    # --- and it is an ordinary item -------------------------------------------
    # Which is the point of resolving rather than special-casing: it saves, it
    # undoes, it evaluates, and the field stays exact.
    print(f"  the trimmed document's safe step scale: {below.safe_step_scale():.3f}")
    if below.safe_step_scale() < 0.99:
        raise SystemExit("a trim stopped being an exact field")

    R.export_model(below, "30_trim_curve.ply", resolution=72, decimate=0.08)


if __name__ == "__main__":
    main()
