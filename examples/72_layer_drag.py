"""Dragging a whole layer, without re-evaluating it once.

THE GAP THIS CLOSES, stated as an artist would: grab a subtool with the Move
gizmo and drag it across the model, and the viewport goes to treacle — even
though nothing about the subtool changed. It is the same shape it was; it is
just somewhere else.

The engine did not know that. A layer's transform reaches the compiled field
only as a change of frame inside each item's inverse matrix, so a rigid
placement leaves the layer's surface exactly what it was, moved by one matrix —
and layers combine by hard union, so no OTHER layer has to be re-solved either.
But every frame of a drag set the layer's transform, and every one of those
invalidated the layer's whole box and re-walked every brick in it. Measured on a
1000-item layer over 5832 bricks: 95.7 ms a frame, against 0.30 ms to transform
the finished mesh by the same matrix.

WHAT THE PICTURES SHOW:

  - THE TWO SCOPES A PREVIEW IS DRAWN FROM, each evaluated ONCE: the document
    with the dragged layer excluded, and that layer alone. Each is exact on its
    own because layers hard-union. What the pair cannot show is their mutual
    occlusion where they overlap — that is stated here rather than discovered,
    and it resolves when the drag lands.

  - THREE FRAMES OF THE DRAG. These tiles are ray-marched from a throwaway
    document placed at the gesture's matrix, because this file's renderer
    marches a FIELD. A real host does not do that: it already holds the two
    surfaces above and moves the second by the matrix, which is the whole point.
    The matrix printed beside each frame is the one it would use.

  - THE COMMITTED RESULT, which is where the last frame's matrix said it would
    be.

WHAT THE NUMBERS UNDERNEATH ASSERT: sixty frames record ONE command, an edit
attempted mid-gesture is refused, and an abandoned gesture leaves the placement
it opened with.

Run: python examples/72_layer_drag.py
"""

import numpy as np

import pyclay as clay

import _render as R

TILE = 190
EYE = (3.2, 2.0, 3.6)
# Where the horn layer starts, and so what the gesture's delta is measured from.
OPENED = (0.0, 1.15, 0.0)


def body_and_horn():
    """A body, and a horn sitting on it as its own layer — the thing dragged."""
    doc = clay.Document()

    body = doc.add_sdf_layer("body")
    body.add(clay.Sphere(r=0.78), color="#b8b0a4")
    body.add(clay.Sphere(r=0.5, position=(0, 0.72, 0)),
             blend=clay.Smooth(0.2), color="#c8c0b2")

    # HARD combines inside the dragged layer, deliberately. A layer whose items
    # blend is not similar to itself under a SCALE — the layer's scale
    # multiplies an item's rounding and not its blend radius — so a scaling
    # gesture on such a layer reports "general" and promises nothing. This one
    # scales cleanly, which the report below shows.
    horn = doc.add_sdf_layer("horn")
    horn.add(clay.Cone(h=0.5, r1=0.2, r2=0.03, position=(0.0, 0.15, 0.0)),
             color="#8a5a3c")
    horn.add(clay.Sphere(r=0.09, position=(0.0, -0.28, 0.0)), color="#7a4a30")
    # Standing on the head to begin with, so the drag reads as taking it off
    # and setting it down beside the body.
    doc.set_layer_transform(horn.id, position=OPENED)
    return doc, body, horn


def placed_copy(delta):
    """A throwaway document with the horn moved by the gesture's delta.

    Only so the ray-marcher has a field to march: this file renders a FIELD, and
    a preview frame is not a field a host evaluates. A host draws exactly this
    picture from the two surfaces it already holds, moving the second by the
    same matrix — which is the point of the whole exercise, and why the document
    the gesture is open on has not moved at all.
    """
    doc, _, horn = body_and_horn()
    m = np.array(delta, dtype=np.float32).reshape(4, 4).T
    doc.set_layer_transform(
        horn.id, position=tuple(float(OPENED[i] + m[i, 3]) for i in range(3)))
    return doc


def main():
    R.banner("A layer drag costs one refill, not sixty")
    doc, body, horn = body_and_horn()
    tiles = []

    # -- the two scopes, each evaluated once -------------------------------
    doc.set_layer_visible(horn.id, False)
    tiles.append(R.render_tile(doc, eye=EYE, size=TILE, colors_from_field=True))
    doc.set_layer_visible(horn.id, True)

    doc.set_layer_visible(body.id, False)
    tiles.append(R.render_tile(doc, eye=EYE, size=TILE, colors_from_field=True))
    doc.set_layer_visible(body.id, True)
    print("  the two scopes a preview is drawn from, each evaluated once")

    # -- the classification, before anything moves -------------------------
    rigid = doc.placement_report(horn.id, position=(1.1, 0.5, 0.0))
    scaled = doc.placement_report(horn.id, scale=1.6)
    print(f"  a translation is {rigid['kind']!r}; "
          f"a uniform scale on this layer is {scaled['kind']!r} (x{scaled['scale']:g})")

    # -- the drag ----------------------------------------------------------
    doc.enable_undo()
    before = doc.undo_depth
    frames = 60
    shown = {0, frames // 2, frames - 1}
    previews = []

    with doc.placement_gesture(horn.id) as g:
        for f in range(frames):
            t = (f + 1) / frames
            g.update(position=(OPENED[0] + 1.35 * t,
                              OPENED[1] - 0.75 * t,
                              OPENED[2] + 0.15 * t))
            if f in shown:
                report = g.preview()
                # Rigid: the layer's surface is what it was, moved.
                assert report["kind"] == "rigid", report["kind"]
                previews.append((f, report["delta"]))
            if f == 0:
                # The gesture holds no snapshot, so an edit it cannot reconcile
                # is refused rather than silently interleaved.
                try:
                    body.add(clay.Sphere(r=0.1))
                    raise AssertionError("an edit during a gesture was allowed")
                except ValueError as exc:
                    print(f"  mid-gesture edit refused: {str(exc).split(':')[-1].strip()}")
            # The document has not moved, so nothing is recorded.
            assert doc.undo_depth == before
        # Leaving the block commits: ONE command for the whole drag.

    after = doc.undo_depth
    print(f"  {frames} frames recorded {after - before} command "
          f"({after - before} undo step for the whole drag)")

    for f, delta in previews:
        tiles.append(R.render_tile(placed_copy(delta), eye=EYE, size=TILE,
                                   colors_from_field=True))
        print(f"  frame {f + 1:>2}/{frames}: translation "
              f"({delta[12]:+.3f}, {delta[13]:+.3f}, {delta[14]:+.3f})")

    tiles.append(R.render_tile(doc, eye=EYE, size=TILE, colors_from_field=True))

    # -- an abandoned gesture leaves the placement it opened with ----------
    settled = (OPENED[0] + 1.35, OPENED[1] - 0.75, OPENED[2] + 0.15)
    landed = doc.placement_report(horn.id, position=settled)["delta"][12:15]
    try:
        with doc.placement_gesture(horn.id) as g:
            g.update(position=(9.0, 9.0, 9.0))
            raise RuntimeError("the host abandoned the drag")
    except RuntimeError:
        pass
    still = doc.placement_report(horn.id, position=settled)["delta"][12:15]
    assert np.allclose(landed, still), (landed, still)
    print("  an abandoned gesture left the committed placement untouched")

    R.contact_sheet(tiles, "72_layer_drag.png", columns=3,
                    caption="rest / layer alone / three drag frames / committed")


if __name__ == "__main__":
    main()
