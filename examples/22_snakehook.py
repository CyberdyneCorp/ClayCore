"""Snakehook — pulling horns and tendrils out of a form.

The brush that turns a sphere into a creature, and the last of the core
sculpting brushes with no equivalent here.

**It is not a new kind of geometry, and that is the finding.** This row was
scoped as needing "geometry that grows along a drag rather than a stamp with
fixed support". Checking before writing showed otherwise: the stroke opcode
already sweeps a sphere along a chain with a radius per point, and that *is* a
tendril once the radii taper. A tapered stroke smooth-unioned onto a body reads
as a snakehook, and the document's safe step scale stays at **1.0** — the field
is still exact, unlike a loft or a sweep, so a tendril costs the raymarcher
nothing.

So what this adds is a **resolver**, the same shape as the cut tool: every
ingredient existed, and what did not was the step that turns a gesture into an
item. Leaving that to each caller means each one answers "where does it anchor"
and "how does the radius taper" differently — and a tendril that detaches from
the body, or beads along its length, is what you get.

Two decisions the resolver owns, and both are visible below.

**The tendril begins where the user touched.** The anchor is prepended to the
drag, because those are not the same place: a pick reports the surface, and the
first drag sample arrives a frame later with the finger already moving. Without
it the tendril starts wherever that sample landed.

That replaced a different claim, which was wrong and is worth recording. The
anchor was going to be pushed *inside* the surface, on the theory that one
sitting exactly on it would leave a neck. It leaves nothing: the sweep from a
surface point already overlaps the body by its own radius, so a deeper anchor
only adds material where the body is solid anyway — the field around the base
measured identical at every depth. The parameter came out rather than staying as
a knob that does nothing.

**The taper follows arc length, not sample count.** A hand moves at an uneven
speed, so a drag's samples bunch where it slowed. Tapering by index would let
the speed of the gesture decide the thickness of the tendril — the same defect
the swept opcode had to avoid when distributing profiles. The third section
draws the same path sampled two ways and checks they agree.

And the honest difference from ZBrush: this **adds** material rather than moving
it. ZBrush pulls existing surface, so the body dimples slightly where the
tendril came from. This grows a tendril and leaves the body alone. The
difference shows only at the base, and conserving volume is a different brush
rather than a better snakehook.
"""

import numpy as np

import pyclay as clay

import _render as R


BODY = 0.6


def drag_path(samples=14, curl=1.9, rise=0.85, reach=0.55, twist=0.25):
    """A drag leaving the top of the body and curling over."""
    t = np.linspace(0.0, 1.0, samples)
    return np.stack([reach * np.sin(t * curl), BODY + t * rise, twist * t * t], axis=1
                    ).astype(np.float32)


def body_with(*tendrils, blend=0.09):
    doc = clay.Document()
    layer = doc.add_sdf_layer("l")
    layer.add(clay.Sphere(r=BODY), color="#b0784a")
    for t in tendrils:
        layer.add(t, blend=clay.Smooth(blend), color="#b0784a")
    return doc


def pull(path, **kwargs):
    """Anchor on top of the body, dragging away from it."""
    return clay.snakehook(anchor=(0, BODY, 0), inward=(0, -1, 0), path=path, **kwargs)


def main():
    R.banner("22 snakehook — pulling a tendril out of a form")

    EYE, TARGET = (2.4, 1.5, 2.4), (0, 0.6, 0)

    # --- the taper shapes what you get ---------------------------------------
    path = drag_path()
    print("  the same drag, shaped by the taper curve:")
    tiles = [R.render_array(body_with(), eye=EYE, target=TARGET, width=205, height=195)]
    labels = ["no tendril"]
    for name, curve in (("horn", 0.4), ("linear", 1.0), ("whip", 3.0)):
        doc = body_with(pull(path, base_radius=0.22, taper_curve=curve))
        print(f"    {name:<7} curve {curve:<4} -> step scale {doc.safe_step_scale():.3f}")
        if doc.safe_step_scale() < 0.999:
            raise SystemExit(f"a {name} tendril stopped being exact")
        tiles.append(R.render_array(doc, eye=EYE, target=TARGET, width=205, height=195))
        labels.append(f"{name} (curve {curve})")
    R.contact_sheet(tiles, "22_snakehook_taper.png", columns=4, caption=", ".join(labels))

    # --- the tendril begins where the user touched ---------------------------
    # A real drag's first sample is not the picked point: the pick reports the
    # surface, the first sample arrives a frame later with the finger moving.
    late = path[1:]                                  # as if the first sample were lost
    with_anchor = pull(late, base_radius=0.22)
    without = clay.Stroke(points=np.concatenate(
        [late, np.full((len(late), 1), 0.22, np.float32)], axis=1),
        types="spline", tolerance=0.01)
    print(f"  the resolved tendril starts at y = {with_anchor.points[0][1]:.3f} "
          f"(the picked point), not {late[0][1]:.3f} (the first sample)")
    if abs(with_anchor.points[0][1] - BODY) > 1e-4:
        raise SystemExit("the tendril did not begin at the anchor")

    R.contact_sheet(
        [R.render_array(body_with(with_anchor), eye=(1.6, 1.1, 1.6), target=(0.15, 0.75, 0),
                        width=310, height=290),
         R.render_array(body_with(without), eye=(1.6, 1.1, 1.6), target=(0.15, 0.75, 0),
                        width=310, height=290)],
        "22_snakehook_anchor.png", columns=2,
        caption="with the anchor prepended, and with the raw drag only — same blend, "
                "so the difference is the anchor and nothing else")

    # --- arc length, not sample count ----------------------------------------
    # The same path, sampled evenly and with the points bunched at the start,
    # as a hand that hesitated would leave them.
    t_even = np.linspace(0.0, 1.0, 14)
    t_bunched = np.concatenate([np.linspace(0.0, 0.12, 7), np.linspace(0.12, 1.0, 7)])
    def along(ts):
        return np.stack([0.55 * np.sin(ts * 1.9), BODY + ts * 0.85, 0.25 * ts * ts],
                        axis=1).astype(np.float32)

    a = body_with(pull(along(t_even), base_radius=0.22))
    b = body_with(pull(along(t_bunched), base_radius=0.22))
    ts = np.linspace(0.0, 1.0, 60)
    probes = along(ts) + np.array([0.12, 0.0, 0.0], np.float32)
    worst = float(np.abs(a.eval(probes) - b.eval(probes)).max())
    print(f"  an even drag and a bunched one differ by at most {worst:.4f}")
    if worst > 0.03:
        raise SystemExit("the taper is following sample count, not arc length")

    # --- a tap still leaves a mark -------------------------------------------
    tap = np.array([[0, BODY, 0], [0, BODY + 0.01, 0]], np.float32)
    doc = body_with(pull(tap, base_radius=0.18))
    marked = bool(doc.eval(np.array([[0, BODY + 0.05, 0]], np.float32))[0] < 0)
    print(f"  a drag shorter than a step still leaves a mark: {marked}")
    if not marked:
        raise SystemExit("a tap produced nothing")

    # --- degenerate input is refused -----------------------------------------
    for name, kwargs in (("an empty drag", dict(path=np.zeros((0, 3), np.float32))),
                         ("a zero base radius", dict(path=path, base_radius=0.0))):
        try:
            pull(**kwargs)
            raise SystemExit(f"{name} should have been refused")
        except ValueError:
            print(f"  {name} is refused")

    # --- a creature -----------------------------------------------------------
    # Several tendrils on one body: the point of the brush.
    rng = np.random.default_rng(6)
    doc = clay.Document()
    layer = doc.add_sdf_layer("l")
    layer.add(clay.Sphere(r=BODY), color="#b0784a")
    for i in range(7):
        a0 = 2 * np.pi * i / 7 + 0.2
        lean = 0.55 + 0.25 * rng.random()
        up = np.array([np.cos(a0) * 0.62, 0.35 + 0.5 * rng.random(), np.sin(a0) * 0.62])
        up /= np.linalg.norm(up)
        t = np.linspace(0, 1, 12)[:, None]
        curl = np.array([np.cos(a0) * 0.45, 0.15, np.sin(a0) * 0.45]) * lean
        pts = (up * BODY + up * t * 0.55 + curl * t * t).astype(np.float32)
        layer.add(clay.snakehook(anchor=tuple(up * BODY), inward=tuple(-up), path=pts,
                                 base_radius=0.15, tip_fraction=0.1, taper_curve=0.6),
                  blend=clay.Smooth(0.07), color="#b0784a")
    print(f"  seven tendrils on one body, step scale {doc.safe_step_scale():.3f}")
    R.render(doc, "22_snakehook_creature.png", eye=(2.6, 1.4, 2.6), target=(0, 0.3, 0),
             caption="seven tendrils pulled from one sphere — still an exact field")

    R.export_model(doc, "22_snakehook.ply", resolution=96, decimate=0.08)


if __name__ == "__main__":
    main()
