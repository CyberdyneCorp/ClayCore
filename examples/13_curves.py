"""Control-point curves: a stroke is a curve whose points are all hard corners.

A stroke point carries a *type* saying how it joins the next one — a hard
corner (a straight segment, and what a finger drag produces), a Catmull-Rom
spline through the points, an approximating B-spline that rounds corners off,
or a Bezier shaped by two handles. Typed points are tessellated into the same
segment chain the engine already evaluates, at compile time.

That is the whole design, and it is why curves were affordable. A new `Curve`
primitive would have meant a spline evaluator running per sample in the inner
loop of every raycast, four backend implementations, a parity corpus row and
fresh exactness analysis. Lowering into the existing chain instead means a
curve costs nothing at evaluation time and inherits everything: the four
backends, per-brick culling, picking, undo, masks and the file format.

Three things this example asserts rather than shows:

  * An all-hard chain evaluates *bit-identically* to what it did before types
    existed, so this was not a migration.
  * Bounds come from the tessellated curve, not the control points — a Bezier
    can leave its control hull entirely, and a bound that missed that would
    have culling drop the arc and picking miss it.
  * The tolerance is a property of the *document*, not the viewer. Two builds
    have to agree on what a document means.
"""

import numpy as np

import pyclay as clay

import _render as R


def ring(n=6, radius=1.0, tube=0.09, y=0.0):
    """Control points evenly around a circle."""
    a = np.linspace(0.0, 2.0 * np.pi, n, endpoint=False)
    return np.stack([np.cos(a) * radius, np.full(n, y), np.sin(a) * radius,
                     np.full(n, tube)], axis=1).astype(np.float32)


def main():
    R.banner("13 curves — one point list, four ways of joining it")

    pts = ring()

    # --- the four point types, same control points ---------------------------
    tiles = []
    for kind in ("hard", "spline", "bspline", "bezier"):
        doc = clay.Document()
        layer = doc.add_sdf_layer("curve")
        kwargs = {}
        if kind == "bezier":
            # Tangential handles, so the Bezier reproduces a smooth ring: the
            # handle is a quarter-turn's worth along the tangent at each point.
            a = np.arctan2(pts[:, 2], pts[:, 0])
            k = 0.55 * (2.0 * np.pi / len(pts))
            tangent = np.stack([-np.sin(a), np.zeros(len(pts)), np.cos(a)], axis=1) * k
            kwargs["out_handles"] = tangent.astype(np.float32)
            kwargs["in_handles"] = (-tangent).astype(np.float32)
        layer.add(clay.Stroke(points=pts, types=kind, closed=True, tolerance=0.004,
                              **kwargs), color="#c08a4a")
        # Whether the curve actually passes through its control points is the
        # difference between the interpolating types and the approximating
        # one, and it is what a user picking a type is choosing between.
        through = int((doc.eval(pts[:, :3].copy()) < 0).sum())
        print(f"  {kind:8s} passes through {through}/{len(pts)} control points")
        tiles.append(R.render_array(doc, eye=(2.2, 2.4, 2.6), target=(0, 0, 0), width=210,
                                    height=195))
    R.contact_sheet(tiles, "13_point_types.png", columns=4,
                    caption="the same six control points as hard, spline, bspline, bezier")

    # --- a hard chain is exactly the stroke it always was --------------------
    plain = clay.Document()
    plain.add_sdf_layer("l").add(clay.Stroke(points=pts))
    typed = clay.Document()
    typed.add_sdf_layer("l").add(clay.Stroke(points=pts, types="hard"))
    probes = np.random.default_rng(4).uniform(-2, 2, size=(4096, 3)).astype(np.float32)
    identical = bool(np.array_equal(plain.eval(probes), typed.eval(probes)))
    print(f"  an all-hard chain is bit-identical to an untyped one: {identical}")
    if not identical:
        raise SystemExit("typing the points changed an all-hard chain")

    # --- tolerance -----------------------------------------------------------
    # Measured against the finest tessellation of the SAME curve, not against a
    # circle: the spline through five points is not a circle, and comparing to
    # one would report that error rather than the tessellation's.
    def field(tolerance):
        doc = clay.Document()
        doc.add_sdf_layer("l").add(
            clay.Stroke(points=ring(n=5, tube=0.12), types="spline", closed=True,
                        tolerance=tolerance), color="#4a80c0")
        return doc

    a = np.linspace(0.0, 2.0 * np.pi, 512, endpoint=False)
    near_curve = np.stack([np.cos(a), np.zeros(512), np.sin(a)], axis=1).astype(np.float32)
    reference = field(0.0005).eval(near_curve)

    tiles = []
    for tolerance in (0.2, 0.05, 0.01, 0.002):
        doc = field(tolerance)
        worst = float(np.abs(doc.eval(near_curve) - reference).max())
        print(f"  tolerance {tolerance:<6} -> worst departure from the finest curve {worst:.4f}")
        tiles.append(R.render_array(doc, eye=(0.4, 3.2, 0.4), target=(0, 0, 0), width=200,
                                    height=190))
    R.contact_sheet(tiles, "13_tolerance.png", columns=4,
                    caption="the same curve at tolerance 0.2, 0.05, 0.01, 0.002")

    # --- bounds cover the arc, not the control points ------------------------
    # Both control points sit at y = 0; the handles carry the span to y = 1.5.
    two = np.array([[-1, 0, 0, 0.12], [1, 0, 0, 0.12]], np.float32)
    handles_out = np.array([[0, 2, 0], [0, 0, 0]], np.float32)
    handles_in = np.array([[0, 0, 0], [0, 2, 0]], np.float32)
    arc = clay.Document()
    arc.add_sdf_layer("l").add(clay.Stroke(points=two, types="bezier",
                                           in_handles=handles_in, out_handles=handles_out),
                               color="#7aa06a")
    peak = float(arc.eval(np.array([[0, 1.5, 0]], np.float32))[0])
    hit = arc.raycast((0, 1.5, -4), (0, 0, 1))
    print(f"  the arc peaks at y=1.5 where both control points sit at y=0 "
          f"(field {peak:.3f}), and a ray finds it: {hit is not None}")
    if not (peak < 0 and hit is not None):
        raise SystemExit("bounds were taken from the control points, not the curve")
    R.render(arc, "13_bezier_arc.png", eye=(0.5, 1.4, 3.4), target=(0, 0.7, 0),
             caption="a Bezier span leaving its control points entirely")

    # --- editing a placed curve is an ordinary edit --------------------------
    doc = clay.Document()
    doc.enable_undo()
    layer = doc.add_sdf_layer("l")
    node = layer.add(clay.Stroke(points=pts, types="spline", closed=True), color="#c08a4a")
    probe = np.array([[0.0, 0.0, 1.05]], np.float32)
    smooth = float(doc.eval(probe)[0])
    layer.set_points(node, pts, types="hard", closed=True)
    hard = float(doc.eval(probe)[0])
    doc.undo()
    restored = float(doc.eval(probe)[0])
    print(f"  editing the curve is one undoable step: {smooth:.3f} -> {hard:.3f} -> "
          f"{restored:.3f}")
    if restored != smooth:
        raise SystemExit("undoing the curve edit did not restore it exactly")

    R.export_model(doc, "13_curve.ply", resolution=64)


if __name__ == "__main__":
    main()
