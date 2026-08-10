"""The Move brush — dragging the assembled surface, not one item of it.

ZBrush's Move drags the **surface**. Putting a `grab` deformer on an item does
not, and the gap between those two sentences is the whole of this example.

**A deformer is per item, and its centre is in that item's own frame.** So a
grab drags one item's field. On a form smooth-unioned from several — which is
what a blocked-out sculpt is — grabbing one pulls its share and leaves the rest
behind. Nothing errors; it just looks wrong, and the first section measures it.

**The fix is to apply the same warp to every contributing item**, each mapped
into its own frame. That is not an approximation of warping their combination,
it *is* warping their combination: combine ops are pointwise in the deformed
point, so `op(f(W(p)), g(W(p))) == (op(f,g))(W(p))`. Transform's scale is
uniform by design, so a spherical falloff stays spherical rather than becoming
an ellipsoid.

**And the warp goes at the front of the chain.** Deformers apply in authoring
order, so `deformers[0]` is the outermost warp on the geometry. Appended at the
back instead, a grab's region weight is read at a point the earlier deformers
already moved, and the drag acts somewhere other than where it was aimed.

One thing the brush does *not* fix, and says so rather than hiding: the surface
moves **less** than the displacement asked for. `grab` samples where material
came from using the weight at the sample point rather than at its preimage. The
pull is monotonic, so a UI can calibrate against it; solving for the true
preimage would cost an iteration per sample and buy nothing a sculptor can feel.
The last section measures that too.
"""

import numpy as np

import pyclay as clay

import _render as R

GAP = 0.45


def blended_form(grab_left=None):
    """Two balls smooth-unioned into one form.

    `grab_left` puts a raw grab on the LEFT item only — the naive Move.
    """
    doc = clay.Document()
    layer = doc.add_sdf_layer("form")
    for x in (-GAP, GAP):
        ball = clay.Sphere(0.5).at((x, 0, 0))
        if grab_left is not None and x < 0:
            ball.grab(**grab_left)
        layer.add(ball, blend=clay.Smooth(0.25), color="#7f93a8")
    return doc, layer


def top(doc, x):
    """Where the surface sits above x, marching in from outside."""
    ys = np.arange(1.6, -1.6, -0.002, dtype=np.float32)
    pts = np.stack([np.full_like(ys, x), ys, np.zeros_like(ys)], axis=1)
    inside = np.nonzero(doc.eval(pts) <= 0.0)[0]
    return float(ys[inside[0]]) if len(inside) else float("nan")


def main():
    R.banner("26 move brush — dragging the surface, not one item of it")

    # Straight on, down -Z: the drag is in the XY plane, so a front view is
    # the one where "symmetric about the centre" is something you can see
    # rather than only read in the numbers.
    EYE, TARGET = (0.0, 0.35, 3.1), (0.0, 0.1, 0.0)
    PROBES = (-GAP, 0.0, GAP)

    base, _ = blended_form()
    before = {x: top(base, x) for x in PROBES}
    print(f"  the form's top: " + "  ".join(f"x={x:+.2f} {before[x]:.3f}" for x in PROBES))

    # --- what a grab on one item actually does -------------------------------
    RADIUS = 0.55
    naive, _ = blended_form(grab_left=dict(center=(0, 0.45, 0), radius=RADIUS,
                                           displacement=(0, 0.4, 0)))
    naive_lift = {x: top(naive, x) - before[x] for x in PROBES}
    print("  a grab on the LEFT item only:")
    for x in PROBES:
        print(f"    x={x:+.2f} lifts {naive_lift[x]:+.3f}")
    if not (naive_lift[-GAP] > 0.02 and abs(naive_lift[GAP]) < 0.005):
        raise SystemExit("a per-item grab was supposed to leave the other item behind")

    # --- what the Move brush does --------------------------------------------
    doc, layer = blended_form()
    touched = layer.move_surface((0, 0.45, 0), (0, 0.4, 0), radius=RADIUS)
    moved_lift = {x: top(doc, x) - before[x] for x in PROBES}
    print(f"  move_surface warped {len(touched)} items:")
    for x in PROBES:
        print(f"    x={x:+.2f} lifts {moved_lift[x]:+.3f}")
    if len(touched) != 2:
        raise SystemExit("both items should have taken a share of the drag")
    if abs(moved_lift[-GAP] - moved_lift[GAP]) > 0.005:
        raise SystemExit("the drag was centred, so the result should be symmetric")
    if moved_lift[0.0] < max(moved_lift[-GAP], moved_lift[GAP]):
        raise SystemExit("the lift should peak at the drag's own centre")

    R.contact_sheet(
        [R.render_array(base, eye=EYE, target=TARGET, width=230, height=215),
         R.render_array(naive, eye=EYE, target=TARGET, width=230, height=215),
         R.render_array(doc, eye=EYE, target=TARGET, width=230, height=215)],
        "26_move_vs_grab.png", columns=3,
        caption="the form, a grab on one item (one side rises), and move_surface "
                "(the surface rises as one)")

    # --- the drag is a world-space gesture -----------------------------------
    # A grab's own centre is local, which is the trap this brush exists to
    # remove: aim at where the form IS, in world units.
    off, off_layer = blended_form()
    off_touched = off_layer.move_surface((40.0, 0, 0), (0, 0.4, 0), radius=RADIUS)
    print(f"  a drag 40 units away warps {len(off_touched)} items "
          f"(items out of reach are skipped, not given a no-op deformer)")
    if off_touched:
        raise SystemExit("a drag that cannot reach anything should warp nothing")

    # --- the pull is monotonic, and short ------------------------------------
    print("  grab weights at the sample point, not its preimage, so the pull "
          "falls short:")
    lifts = []
    for d in (0.1, 0.2, 0.4, 0.8):
        each, each_layer = blended_form()
        each_layer.move_surface((0, 0.45, 0), (0, d, 0), radius=RADIUS)
        lift = top(each, 0.0) - before[0.0]
        lifts.append(lift)
        print(f"    asked for {d:.2f} -> moved {lift:.3f}  ({lift / d * 100:.0f}%)")
    if not all(a < b for a, b in zip(lifts, lifts[1:])):
        raise SystemExit("the pull stopped being monotonic in the displacement")
    if not all(lift < d for lift, d in zip(lifts, (0.1, 0.2, 0.4, 0.8))):
        raise SystemExit("the pull is supposed to fall short of the displacement")

    tiles = []
    for d in (0.0, 0.3, 0.6):
        each, each_layer = blended_form()
        if d > 0.0:
            each_layer.move_surface((0, 0.45, 0), (0, d, 0), radius=RADIUS)
        tiles.append(R.render_array(each, eye=EYE, target=TARGET, width=230, height=215))
    R.contact_sheet(tiles, "26_move_sweep.png", columns=3,
                    caption="the same drag at 0.0, 0.3 and 0.6 — the pull grows "
                            "monotonically and falls short of what was asked")

    # --- a drag takes everything in its radius, unless you say otherwise -----
    # Widen the region past the form and the far side travels too: the underside
    # is pulled up with the top and the shape hollows out. That is what a
    # region warp does, and `front_only` is the answer — it gates the pull on the
    # half-space the drag heads into.
    wide, wide_layer = blended_form()
    wide_layer.move_surface((0, 0, 0), (0, 0.4, 0), radius=1.1)
    gated, gated_layer = blended_form()
    gated_layer.move_surface((0, 0, 0), (0, 0.4, 0), radius=1.1, front_only=True)

    def bottom(doc, x=0.0):
        ys = np.arange(-1.6, 1.6, 0.002, dtype=np.float32)
        pts = np.stack([np.full_like(ys, x), ys, np.zeros_like(ys)], axis=1)
        inside = np.nonzero(doc.eval(pts) <= 0.0)[0]
        return float(ys[inside[0]])

    base_bottom = bottom(base)
    print(f"  a radius past the form drags its underside too: "
          f"{base_bottom:.3f} -> {bottom(wide):.3f}")
    print(f"  ...and front_only leaves it where it was:        "
          f"{base_bottom:.3f} -> {bottom(gated):.3f}")
    if bottom(wide) - base_bottom < 0.02:
        raise SystemExit("a wide drag was supposed to pull the far side up too")
    if abs(bottom(gated) - base_bottom) > 0.01:
        raise SystemExit("front_only was supposed to leave the far side alone")

    R.contact_sheet(
        [R.render_array(base, eye=EYE, target=TARGET, width=230, height=215),
         R.render_array(wide, eye=EYE, target=TARGET, width=230, height=215),
         R.render_array(gated, eye=EYE, target=TARGET, width=230, height=215)],
        "26_move_front_only.png", columns=3,
        caption="a drag wider than the form: the underside travels with the top, "
                "until front_only gates it")

    # --- one gesture, one undo step ------------------------------------------
    undoable, undo_layer = blended_form()
    undoable.enable_undo()
    undo_layer.move_surface((0, 0.45, 0), (0, 0.4, 0), radius=RADIUS)
    print(f"  a drag touching 2 items is {undoable.undo_depth} undo step")
    if undoable.undo_depth != 1:
        raise SystemExit("a drag is one gesture and should undo in one step")
    undoable.undo()
    if abs(top(undoable, 0.0) - before[0.0]) > 1e-4:
        raise SystemExit("undoing the drag did not restore the form")
    print("  ...and undoing it puts the form back exactly")

    # --- preview is PURE: it says what a drag would touch, and touches nothing --
    # docs/07 lists move_surface_preview beside move_surface as the reason a
    # host can show what a drag is about to affect. Nothing measured it, so the
    # two could have drifted apart — and a preview that disagrees with the edit
    # is worse than none, because the host would highlight the wrong items.
    pre_doc, pre_layer = blended_form()
    probe = np.array([[x, 0.5, 0.0] for x in PROBES], dtype=np.float32)
    untouched = pre_doc.eval(probe)

    would = pre_layer.move_surface_preview((0, 0.45, 0), (0, 0.4, 0), radius=RADIUS)
    if not np.allclose(pre_doc.eval(probe), untouched, atol=0):
        raise SystemExit("move_surface_preview changed the document — it must be pure")
    did = pre_layer.move_surface((0, 0.45, 0), (0, 0.4, 0), radius=RADIUS)
    if sorted(would) != sorted(did):
        raise SystemExit(f"preview said {sorted(would)} but the move warped {sorted(did)}")
    print(f"  move_surface_preview named the same {len(would)} items the move warped, "
          f"without touching the document")

    # A drag that cannot reach the form names nothing, rather than naming every
    # item with a zero-weight warp.
    far_doc, far_layer = blended_form()
    if far_layer.move_surface_preview((40.0, 0, 0), (0, 0.4, 0), radius=RADIUS):
        raise SystemExit("a preview out of reach still named items")
    print("  ...and names nothing when the drag cannot reach the form")

    R.export_model(doc, "26_move.ply", resolution=72)


if __name__ == "__main__":
    main()
