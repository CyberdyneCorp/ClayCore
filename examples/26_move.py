"""The Move brush — dragging the surface, not one item.

The `grab` deformer has been here since `add-tape-deformers`, and mapping
ZBrush's Move onto it looks obvious. It is wrong in a way nothing errors on, and
this row exists because of that.

A deformer is emitted into the tape **per item**, and applied to that item's
**local** point. So `grab` drags one item's own field, and its centre is in the
item's frame — a centre of `(0, 0, 0)` grabs the middle of a sphere sitting at
world `x = 1.5`, and a centre of `(1.5, 0, 0)` does nothing to it. On a form that
IS one item those two facts never surface. On a form smooth-unioned from several
— the normal case for a blocked-out sculpt — grabbing one item pulls its share
and leaves the rest behind, which the first section shows.

`Document.grab` is a **resolver**, the same shape as the cut tool and snakehook:
a world drag in, a plan of ordinary edits out. Three things are worth reading
before looking.

**It is exact, not an approximation.** Combine ops are pointwise in the deformed
point, so warping every operand identically is the same as warping their
combination — applying one world warp to every item *is* a field-level grab.
And `Transform`'s scale is uniform by design, so a spherical falloff stays
spherical and the world-to-local mapping is always exactly expressible.

**A drag coalesces.** During one drag the centre and radius are fixed and only
the displacement grows. Appending a deformer per frame would grow the chain
without bound and cost exactness with every frame, so a trailing grab with the
same centre and radius is replaced instead. The third section drags in ten
frames and checks the result equals a single drag of the same distance.

**It only touches what it reaches.** A grab breaks exactness and raises the
Lipschitz bound, so putting one on every item would cost a whole document its
step scale for a local gesture. Items whose influence bound misses the drag are
skipped — they are provably unaffected, since outside its radius the warp is the
identity.
"""

import numpy as np

import pyclay as clay

import _render as R


def blob(deform_one=False):
    """A form built from three overlapping balls — one surface, three items."""
    doc = clay.Document()
    layer = doc.add_sdf_layer("l")
    for i, x in enumerate((-0.5, 0.0, 0.5)):
        ball = clay.Sphere(r=0.38, position=(x, 0, 0))
        if deform_one and i == 0:
            # The obvious-looking thing, and the wrong one: local (0.5,0.2,0) on
            # an item at world (-0.5,0,0) IS the world drag centre below.
            ball.grab(center=(0.5, 0.2, 0), radius=1.1, displacement=(0, 0.32, 0))
        layer.add(ball, color="#b0784a", blend=clay.Smooth(0.12))
    return doc


def top_at(doc, x, z=0.0):
    """Where the surface crosses, scanning DOWN from above the form."""
    ys = np.arange(1.6, -1.2, -0.002, dtype=np.float32)
    pts = np.stack([np.full_like(ys, x), ys, np.full_like(ys, z)], axis=1)
    inside = np.nonzero(doc.eval(pts) <= 0)[0]
    return float(ys[inside[0]]) if len(inside) else float("nan")


def main():
    R.banner("26 move — dragging the surface, not one item")

    EYE, TARGET = (0.2, 0.9, 3.4), (0, 0.05, 0)

    # --- one item, or the surface --------------------------------------------
    plain = blob()
    one_item = blob(deform_one=True)
    surface = blob()
    reached = surface.grab(centre=(0, 0.2, 0), radius=1.1, displacement=(0, 0.32, 0))
    print(f"  the same world drag reached {reached} items through the resolver")

    print("  surface height across the form:")
    print(f"    {'x':>6}  {'plain':>7}  {'one item':>9}  {'resolver':>9}")
    for x in (-0.75, -0.5, 0.0, 0.5, 0.75):
        print(f"    {x:>6.2f}  {top_at(plain, x):>7.3f}  {top_at(one_item, x):>9.3f}  "
              f"{top_at(surface, x):>9.3f}")

    def tilt(doc):
        return ((top_at(doc, -0.5) - top_at(plain, -0.5))
                - (top_at(doc, 0.5) - top_at(plain, 0.5)))
    print(f"  left minus right: {tilt(one_item):+.3f} grabbing one item, "
          f"{tilt(surface):+.3f} grabbing the surface")
    if not (tilt(one_item) > 0.04 and abs(tilt(surface)) < 0.01):
        raise SystemExit("the resolver is no longer moving the form as one surface")

    R.contact_sheet(
        [R.render_array(plain, eye=EYE, target=TARGET, width=205, height=195),
         R.render_array(one_item, eye=EYE, target=TARGET, width=205, height=195),
         R.render_array(surface, eye=EYE, target=TARGET, width=205, height=195)],
        "26_move_one_item_vs_surface.png", columns=3,
        caption="before, the grab deformer on one item, and the same world drag "
                "through the resolver — the middle one pulls one ball out of the form")

    # --- it does not care how the transform is split --------------------------
    # The strongest statement of correctness available without reimplementing
    # the warp here: the same world surface, placed two different ways, must come
    # back the same after the same world drag.
    def one_ball(on_layer):
        doc = clay.Document()
        layer = doc.add_sdf_layer("l")
        if on_layer:
            doc.set_layer_transform(layer.id, position=(0.6, -0.2, 0.15),
                                    rotation_axis_angle=((0.3, 1.0, 0.2), 0.7), scale=1.4)
            layer.add(clay.Sphere(r=0.5), color="#b0784a")
        else:
            layer.add(clay.Sphere(r=0.5, position=(0.6, -0.2, 0.15),
                                  rotation_axis_angle=((0.3, 1.0, 0.2), 0.7), scale=1.4),
                      color="#b0784a")
        doc.grab(centre=(0.6, 0.3, 0.15), radius=0.9, displacement=(0.2, 0.35, -0.1))
        return doc

    rng = np.random.default_rng(7)
    probes = rng.uniform(-1.4, 2.0, size=(6000, 3)).astype(np.float32)
    gap = float(np.abs(one_ball(False).eval(probes) - one_ball(True).eval(probes)).max())
    print(f"  the same drag on the same surface, placed on the node or on the "
          f"layer, differs by {gap:.2e}")
    if gap > 1e-4:
        raise SystemExit("the world-to-local mapping depends on how the transform is split")

    # --- a drag coalesces -----------------------------------------------------
    stepped = blob()
    for i in range(1, 11):
        stepped.grab(centre=(0, 0.2, 0), radius=1.1, displacement=(0, 0.32 * i / 10.0, 0))
    once = blob()
    once.grab(centre=(0, 0.2, 0), radius=1.1, displacement=(0, 0.32, 0))
    drift = float(np.abs(stepped.eval(probes) - once.eval(probes)).max())
    print(f"  ten drag frames vs one drag of the same distance: fields differ by "
          f"{drift:.2e}, step scale {stepped.safe_step_scale():.3f} vs "
          f"{once.safe_step_scale():.3f}")
    if drift > 1e-6 or abs(stepped.safe_step_scale() - once.safe_step_scale()) > 1e-6:
        raise SystemExit("a drag is piling deformers up instead of coalescing")

    # --- it reaches only what it can touch ------------------------------------
    wide = blob()
    wide.add_sdf_layer("far").add(clay.Sphere(r=0.3, position=(6, 0, 0)), color="#7f8a94")
    plan = wide.grab_preview(centre=(0, 0.2, 0), radius=1.1, displacement=(0, 0.32, 0))
    print(f"  with a ball six units away in the document, the drag plans "
          f"{len(plan)} edits, not {len(plan) + 1}")
    if len(plan) != 3:
        raise SystemExit("culling by influence bound stopped working")

    # And previewing really is free of side effects.
    before = wide.eval(probes)
    wide.grab_preview(centre=(0, 0.2, 0), radius=1.1, displacement=(0, 0.32, 0))
    if not np.array_equal(wide.eval(probes), before):
        raise SystemExit("previewing a drag modified the document")

    # --- a gesture is one undo step -------------------------------------------
    doc = blob()
    doc.enable_undo()
    reached = doc.grab(centre=(0, 0.2, 0), radius=1.1, displacement=(0, 0.32, 0))
    steps, lifted = doc.undo_depth, top_at(doc, 0.0)
    doc.undo()
    print(f"  one gesture reached {reached} items and is {steps} undo step; undoing "
          f"put the surface back at {top_at(doc, 0.0):.3f} from {lifted:.3f}")
    if steps != 1:
        raise SystemExit("a Move is one gesture and should be one undo step")
    if abs(top_at(doc, 0.0) - top_at(plain, 0.0)) > 1e-3:
        raise SystemExit("undoing a grab did not restore the surface")

    # --- front_only, seen from above -----------------------------------------
    # Viewed side-on a drag toward the camera is invisible, so this section looks
    # straight down: +Z now reads. `front_only` gates the pull on the half-space
    # it heads into, so the far side of a form does not travel with the near one.
    TOP = dict(eye=(0.15, 3.2, 0.5), target=(0, 0, 0))
    tiles, labels = [], []
    for name, disp, front in (("plain", None, False),
                              ("toward", (0, 0, 0.45), False),
                              ("toward (front only)", (0, 0, 0.45), True),
                              ("sideways", (0.45, 0, 0), False)):
        d = blob()
        if disp is not None:
            d.grab(centre=(0, 0, 0), radius=1.0, displacement=disp, front_only=front)
        tiles.append(R.render_array(d, width=205, height=195, **TOP))
        labels.append(name)
    R.contact_sheet(tiles, "26_move_directions.png", columns=4,
                    caption="from above: " + ", ".join(labels) +
                            " — front_only moves the near half and leaves the far "
                            "side where it was")

    # A drag large against its radius FOLDS space, and the surface creases. That
    # is what a grab is, in any tool; it is not an artifact of resolving one.
    folded = blob()
    folded.grab(centre=(0, 0, 0), radius=0.5, displacement=(0, 0.6, 0))
    gentle = blob()
    gentle.grab(centre=(0, 0, 0), radius=1.4, displacement=(0, 0.6, 0))
    print(f"  the same 0.6 drag through a 0.5 radius vs a 1.4 one: step scale "
          f"{folded.safe_step_scale():.3f} vs {gentle.safe_step_scale():.3f}")
    if folded.safe_step_scale() >= gentle.safe_step_scale():
        raise SystemExit("a tighter drag stopped costing the marcher more")

    R.export_model(surface, "26_move.ply", resolution=72, decimate=0.08)


if __name__ == "__main__":
    main()
