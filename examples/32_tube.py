"""The Tube tool — a drawn path becomes a rope, a pipe or a tentacle.

Nomad Sculpt's Tubes, and the clearest case in this repository of a tool that is
almost entirely *joining things that were already here*:

- the smooth/sharp toggle is `StrokePointType`, already hard / Catmull-Rom /
  B-spline / Bezier, tessellated to a document tolerance
- a round tube with a varying radius is the stroke opcode, which sweeps a sphere
  along a segment chain with a radius PER POINT
- a cross-section that is not a circle is `Prim::swept`, carrying profiles along
  a guide on parallel-transported frames
- a closed tube is `stroke_closed`

What did not exist is the step that turns a drawn path plus a few settings into
the right item. Left to each caller, every one answers "how does the radius vary
along the curve", "when is this a stroke and when a sweep" and "what does closed
mean for a tapered tube" differently — the argument the cut tool and snakehook
were built on.

Three things are worth reading before looking.

**The cross-section chooses the representation, and one of them is free.** With
no profile a tube is a swept SPHERE, which is an exact distance field: the safe
step scale stays 1.0, and it costs the raymarcher nothing. Ask for a square and
it becomes a swept item — a bound field, step scale 0.56 in the third section.
That is the real price of a non-circular cross-section, and it is charged where
you choose it rather than discovered later.

**The taper follows arc length, not point index.** Nomad's three radius handles
are start, middle and end; distributing them by index would make the same drawn
shape taper differently depending on how evenly it happened to be tapped. The
second section draws one path twice — evenly spaced, then bunched — and checks
the radius at the same fraction of the length matches.

**"Validate" is not a thing here.** Nomad's tube stays a live curve until you
convert it to polygons; a tube here is an ordinary item from the start, so it
combines, saves, undoes, picks and meshes like any other. Meshing is what the
meshers already do to any document.
"""

import numpy as np

import pyclay as clay

import _render as R

# A path that bends in three dimensions, so the sweep's frame has to work.
PATH = np.array([[-0.62, -0.18, 0.10],
                 [-0.26, 0.30, -0.12],
                 [0.14, -0.05, 0.16],
                 [0.52, 0.34, -0.08]], np.float32)


def doc_with(prim, colour="#b0784a"):
    doc = clay.Document()
    doc.add_sdf_layer("l").add(prim, color=colour)
    return doc


def thickness_at(doc, point, direction=(0, 0, 1), reach=0.5):
    """How thick the tube is at a point, measured across it."""
    d = np.array(direction, np.float32)
    d = d / np.linalg.norm(d)
    ts = np.arange(-reach, reach, 0.002, dtype=np.float32)
    pts = (np.array(point, np.float32)[None, :] + ts[:, None] * d[None, :]).astype(np.float32)
    inside = np.nonzero(doc.eval(pts) <= 0)[0]
    return float(ts[inside[-1]] - ts[inside[0]]) if len(inside) else 0.0


def main():
    R.banner("32 tube — a drawn path becomes a rope, a pipe or a tentacle")

    EYE, TARGET = (1.5, 1.2, 2.1), (0, 0.05, 0)

    # --- the smooth/sharp toggle is the point type ---------------------------
    print("  the same four points, under each point type:")
    tiles, labels = [], []
    for kind in ("hard", "spline", "bspline"):
        doc = doc_with(clay.tube(PATH, point_type=kind, radius=0.09))
        print(f"    {kind:<8} step scale {doc.safe_step_scale():.3f}")
        tiles.append(R.render_array(doc, eye=EYE, target=TARGET, width=205, height=195))
        labels.append(kind)

    # A B-spline APPROXIMATES its control points, so it passes further from a
    # corner than a curve that interpolates them. That is the difference the
    # toggle exists for, and it is measurable rather than a matter of taste.
    corner = PATH[1]
    probes = np.array([corner], np.float32)
    dist = {k: float(doc_with(clay.tube(PATH, point_type=k, radius=0.09)).eval(probes)[0])
            for k in ("hard", "spline", "bspline")}
    print(f"  the field AT the second control point: " +
          ", ".join(f"{k} {v:+.3f}" for k, v in dist.items()))
    if not (dist["bspline"] > dist["hard"]):
        raise SystemExit("a B-spline stopped passing further from the control point")

    R.contact_sheet(tiles + [R.render_array(doc_with(clay.tube(PATH, radius=0.09, closed=True)),
                                            eye=EYE, target=TARGET, width=205, height=195)],
                    "32_tube_points.png", columns=4,
                    caption=", ".join(labels) + ", and closed — the toggle is the curve's own "
                            "point type, not a second kind of curve")

    # --- the radius varies, by arc length -------------------------------------
    # Measured with HARD points, so the curve passes exactly through them and
    # start/middle/end are places on the tube. A B-spline APPROXIMATES its
    # control points — the section above measures that — so probing at one would
    # measure the smoothing rather than the radius.
    doc = doc_with(clay.tube(PATH, point_type="hard", radius=0.14, radius_mid=0.09,
                             radius_end=0.03))
    print("  a tapered tube, measured across it at its own control points:")
    widths = []
    for name, at in (("start", PATH[0]), ("middle", PATH[1]), ("end", PATH[3])):
        widths.append(thickness_at(doc, at))
        print(f"    {name:<7} {widths[-1]:.3f}")
    if not all(a > b for a, b in zip(widths, widths[1:])):
        raise SystemExit(f"the tube stopped tapering: {widths}")
    # ...and each width really is twice the radius asked for there.
    if abs(widths[0] - 0.28) > 0.03 or abs(widths[-1] - 0.06) > 0.03:
        raise SystemExit(f"the radii are not what was asked for: {widths}")

    # Arc length, not point index: the same shape drawn with bunched control
    # points must taper the same way.
    even = np.array([[t, 0.0, 0.0] for t in np.linspace(-0.6, 0.6, 5)], np.float32)
    bunched = np.array([[t, 0.0, 0.0] for t in (-0.6, -0.5, -0.4, 0.0, 0.6)], np.float32)
    settings = dict(point_type="hard", radius=0.16, radius_mid=0.10, radius_end=0.04)
    a, b = doc_with(clay.tube(even, **settings)), doc_with(clay.tube(bunched, **settings))
    mid = np.array([0.0, 0.0, 0.0], np.float32)
    print(f"  evenly spaced vs bunched control points, thickness at the midpoint: "
          f"{thickness_at(a, mid):.3f} vs {thickness_at(b, mid):.3f}")
    if abs(thickness_at(a, mid) - thickness_at(b, mid)) > 0.02:
        raise SystemExit("the taper follows point index rather than arc length")

    R.contact_sheet(
        [R.render_array(doc_with(clay.tube(PATH, radius=r, radius_mid=m, radius_end=e)),
                        eye=EYE, target=TARGET, width=205, height=195)
         for r, m, e in ((0.09, 0.09, 0.09), (0.14, 0.09, 0.03), (0.03, 0.14, 0.03))],
        "32_tube_radius.png", columns=3,
        caption="uniform, tapering to a point, and fat in the middle — Nomad's three "
                "radius handles, distributed by arc length")

    # --- the cross-section chooses the representation -------------------------
    print("  the cross-section decides what the tube IS:")
    round_doc = doc_with(clay.tube(PATH, radius=0.09))
    print(f"    no profile  -> swept SPHERE, exact, step scale "
          f"{round_doc.safe_step_scale():.3f}")
    tiles, labels = [R.render_array(round_doc, eye=EYE, target=TARGET, width=205, height=195)], \
                    ["round (exact)"]
    for name, profile in (("box", clay.Profile.box(0.09, 0.05)),
                          ("hexagon", clay.Profile.hexagon(0.09))):
        doc = doc_with(clay.tube(PATH, profile=profile))
        print(f"    {name:<11} -> swept item, bound, step scale {doc.safe_step_scale():.3f}")
        tiles.append(R.render_array(doc, eye=EYE, target=TARGET, width=205, height=195))
        labels.append(name)
        if doc.safe_step_scale() >= 1.0:
            raise SystemExit(f"a {name} profile stopped costing the marcher")
    if round_doc.safe_step_scale() < 1.0:
        raise SystemExit("a round tube stopped being exact")
    R.contact_sheet(tiles, "32_tube_profiles.png", columns=3, caption=", ".join(labels) +
                    " — a circle is a swept sphere and free; anything else is a swept item")

    # --- and it is an ordinary item -------------------------------------------
    # Which is the point of resolving rather than special-casing: no "Validate"
    # step, because it was never a live curve waiting to become geometry.
    scene = clay.Document()
    layer = scene.add_sdf_layer("l")
    layer.add(clay.Sphere(r=0.34, position=(-0.62, -0.18, 0.10)), color="#8d6a4f")
    layer.add(clay.tube(PATH, radius=0.13, radius_mid=0.09, radius_end=0.04),
              color="#b0784a", blend=clay.Smooth(0.10))
    layer.add(clay.tube(PATH * np.array([1, -1, 1], np.float32), radius=0.06),
              color="#b0784a", blend=clay.Smooth(0.08))
    print(f"  three items in a document, blended: step scale {scene.safe_step_scale():.3f}")
    R.render(scene, "32_tube_scene.png", eye=(1.7, 1.3, 2.3), target=(0, 0.02, 0),
             caption="a tapered tube and a thin one, blended onto a ball — ordinary items, "
                     "so there is no Validate step")

    R.export_model(scene, "32_tube.ply", resolution=80, decimate=0.08)


if __name__ == "__main__":
    main()
