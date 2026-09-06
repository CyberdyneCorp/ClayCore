"""A layer that cuts the layers below it — A − B + C as a stack.

THE GAP THIS CLOSES, stated as an artist would: a layer was organisation and
nothing else. Every visible SDF layer hard-unioned into the ones beneath it, so
to cut one shape with another you had to put both in the SAME layer — and then
you could no longer hide the cutter, reorder it, transform it, or lock it as a
thing. The cut was a decision you made once, buried in an edit list.

Now a layer carries a COMPOSITION: the operator, blend profile, blend radius and
rounding an item already had, applied to the whole layer's chain. The stack folds
left to right, and layer order and visibility become part of the shape.

WHAT THE PICTURES SHOW:

  - A + C, the form with no cutter in it — which is also, exactly, what the
    fifth tile has to give back when B is hidden.
  - A − B, with B in its own layer set to subtract.
  - A − B + C: three layers, folded in stack order.
  - A + C − B: the SAME three layers, reordered. A different sculpture — this
    is the whole claim that order is geometric.
  - B HIDDEN, which gives back exactly the uncut geometry. Not approximately:
    the numbers below assert the field is bit-identical to a document that
    never carried B at all.
  - A SUBTRACT AT THE BOTTOM OF THE STACK, which shows its own shape rather
    than an empty frame. The FIRST visible SDF layer initialises the
    accumulator and its own operator is not applied, because Subtract and
    Intersect against nothing both produce nothing at all, with no error — and
    an artist who drags their base layer to the top would hit exactly that.

WHAT THE NUMBERS UNDERNEATH ASSERT: two layers and one layer of the same items
are the same field, sample for sample; hiding a subtractive layer restores what
it was cutting exactly; the two orders differ and each survives a save and a
reload unchanged; and a composition a layer cannot carry is refused rather than
stored.

Run: python examples/75_layer_booleans.py
"""

import os
import tempfile

import numpy as np

import pyclay as clay

import _render as R

TILE = 200
EYE = (2.6, 1.9, 3.2)

# Where each of the three forms sits. B cuts a bite out of A's right side; C
# fills part of that bite back in, so the two stack orders cannot agree.
A_R = 0.95
B_R, B_AT = 0.62, (0.72, 0.10, 0.30)
C_R, C_AT = 0.42, (0.95, 0.05, 0.30)


def _probe_points(n=20000, half=1.6, seed=7):
    """A deterministic cloud to compare two documents over."""
    rng = np.random.default_rng(seed)
    return rng.uniform(-half, half, size=(n, 3)).astype(np.float32)


def stacked(order="cut_first", cut_visible=True):
    """A, B(subtract) and C(add) as three layers, in one of the two orders."""
    doc = clay.Document()
    a = doc.add_sdf_layer("A")
    a.add(clay.Sphere(r=A_R), color="#c8b9a0")
    a.add(clay.Sphere(r=0.55, position=(0.0, 0.72, 0.0)),
          blend=clay.Smooth(0.18), color="#d8cbb4")

    def add_b():
        b = doc.add_sdf_layer("B")
        b.add(clay.Sphere(r=B_R, position=B_AT), color="#7d5a44")
        doc.set_layer_composition(b.id, op=clay.Op.SUBTRACT)
        return b

    def add_c():
        c = doc.add_sdf_layer("C")
        c.add(clay.Sphere(r=C_R, position=C_AT), color="#3f7f9f")
        return c

    if order == "cut_first":
        b, c = add_b(), add_c()
    else:
        c, b = add_c(), add_b()
    if not cut_visible:
        doc.set_layer_visible(b.id, False)
    return doc, a, b, c


def flattened():
    """The same shape the old way: ONE layer, the cut spelled on the item.

    A layer holds a CHAIN, so the one-layer equivalent of a composed layer of
    several items is a group carrying the composition. Here each layer holds one
    item, which is the spec's own scenario and the simplest form of the claim.
    """
    doc = clay.Document()
    only = doc.add_sdf_layer("only")
    only.add(clay.Sphere(r=A_R), color="#c8b9a0")
    only.add(clay.Sphere(r=0.55, position=(0.0, 0.72, 0.0)),
             blend=clay.Smooth(0.18), color="#d8cbb4")
    only.add(clay.Sphere(r=B_R, position=B_AT), op=clay.Op.SUBTRACT, color="#7d5a44")
    only.add(clay.Sphere(r=C_R, position=C_AT), color="#3f7f9f")
    return doc


def base_only():
    """A on its own — what hiding B has to give back, exactly."""
    doc = clay.Document()
    a = doc.add_sdf_layer("A")
    a.add(clay.Sphere(r=A_R), color="#c8b9a0")
    a.add(clay.Sphere(r=0.55, position=(0.0, 0.72, 0.0)),
          blend=clay.Smooth(0.18), color="#d8cbb4")
    c = doc.add_sdf_layer("C")
    c.add(clay.Sphere(r=C_R, position=C_AT), color="#3f7f9f")
    return doc


def carving_base():
    """A stack that OPENS with a subtract, which is the reordering trap."""
    doc = clay.Document()
    first = doc.add_sdf_layer("first")
    first.add(clay.Sphere(r=0.8), color="#a05a4a")
    first.add(clay.Box(size=(1.1, 0.5, 1.1), position=(0.0, 0.55, 0.0)),
              blend=clay.Smooth(0.12), color="#b06a58")
    doc.set_layer_composition(first.id, op=clay.Op.SUBTRACT, blend=clay.Smooth(0.2))
    return doc


def main():
    R.banner("75 layer booleans — a layer that cuts the layers below it")
    pts = _probe_points()
    tiles = []

    # -- the form with no cutter, and A - B --------------------------------
    tiles.append(R.render_tile(base_only(), eye=EYE, size=TILE, colors_from_field=True))

    cut, _a, b, _c = stacked("cut_first")
    without_c = clay.Document()  # A - B, for the second tile
    wa = without_c.add_sdf_layer("A")
    wa.add(clay.Sphere(r=A_R), color="#c8b9a0")
    wa.add(clay.Sphere(r=0.55, position=(0.0, 0.72, 0.0)),
           blend=clay.Smooth(0.18), color="#d8cbb4")
    wb = without_c.add_sdf_layer("B")
    wb.add(clay.Sphere(r=B_R, position=B_AT), color="#7d5a44")
    without_c.set_layer_composition(wb.id, op=clay.Op.SUBTRACT)
    tiles.append(R.render_tile(without_c, eye=EYE, size=TILE, colors_from_field=True))

    # -- two layers and one layer are the same field -----------------------
    layered = cut.eval(pts)
    flat = flattened().eval(pts)
    differing = int(np.count_nonzero(layered != flat))
    assert differing == 0, f"{differing} of {len(pts)} samples differ"
    # And the comparison has teeth: without the fold, the two are not equal.
    unioned, _ua, ub, _uc = stacked("cut_first")
    unioned.set_layer_composition(ub.id, op=clay.Op.ADD)
    assert int(np.count_nonzero(unioned.eval(pts) != flat)) > 0
    print(f"  A-B+C as three layers == one layer of the same items: "
          f"{differing} of {len(pts)} samples differ")

    tiles.append(R.render_tile(cut, eye=EYE, size=TILE, colors_from_field=True))

    # -- order is geometry -------------------------------------------------
    other, _oa, _ob, _oc = stacked("add_first")
    order_differs = int(np.count_nonzero(cut.eval(pts) != other.eval(pts)))
    assert order_differs > 0
    print(f"  A-B+C against A+C-B: {order_differs} of {len(pts)} samples differ "
          f"— the same three layers, two sculptures")
    tiles.append(R.render_tile(other, eye=EYE, size=TILE, colors_from_field=True))

    # -- and each order survives a save and a reload -----------------------
    with tempfile.TemporaryDirectory() as tmp:
        for name, doc in (("A-B+C", cut), ("A+C-B", other)):
            path = os.path.join(tmp, "stack.clayspace")
            doc.save(path)
            back = clay.load(path)
            same = int(np.count_nonzero(back.eval(pts) != doc.eval(pts)))
            assert same == 0, f"{name} moved across a reload"
        print("  both orders reload bit-identically, so the stack is part of the file")

    # -- hiding the cutter gives the uncut geometry back -------------------
    hidden, _ha, _hb, _hc = stacked("cut_first", cut_visible=False)
    restored = int(np.count_nonzero(hidden.eval(pts) != base_only().eval(pts)))
    assert restored == 0, f"{restored} samples did not come back"
    print(f"  hiding B restores A+C exactly: {restored} of {len(pts)} samples differ")
    tiles.append(R.render_tile(hidden, eye=EYE, size=TILE, colors_from_field=True))

    # -- the first visible layer initialises, whatever its operator --------
    opening = carving_base()
    solid = int(np.count_nonzero(opening.eval(pts) < 0.0))
    assert solid > 0, "a stack that opens with a subtract showed nothing at all"
    print(f"  a stack opening with Subtract still shows its layer: "
          f"{solid} of {len(pts)} samples are inside it")
    tiles.append(R.render_tile(opening, eye=EYE, size=TILE, colors_from_field=True))

    # -- what a composition refuses ----------------------------------------
    refused = 0
    for bad in ({"op": clay.Op.INLINE}, {"op": clay.Op.TRANSITION_LINEAR},
                {"rounding": -1.0}):
        try:
            cut.set_layer_composition(b.id, **bad)
        except ValueError as exc:
            refused += 1
            print(f"  refused {list(bad)[0]}={list(bad.values())[0]}: {exc}")
    assert refused == 3
    # A voxel or mesh layer refuses one too, at both ends — it has no chain to
    # fold. pyclay hands back a grid rather than a layer id, so that refusal is
    # shown in C (tests/unit/test_layer_composition.cpp) rather than here.

    R.contact_sheet(
        tiles, "75_layer_booleans.png", columns=3,
        caption="A+C / A−B / A−B+C / A+C−B / B hidden (=A+C) / a stack that opens with a cut")


if __name__ == "__main__":
    main()
