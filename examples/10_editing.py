"""Editing a document that already exists.

Everything else in this gallery builds a document and renders it. This one
changes one that is already there: it places items, keeps their ids, and then
moves, recolours, re-blends and removes them — which is what a sculpting UI
spends its time doing.

Every edit here goes through the engine's command vocabulary, the same one the
`.clayspace` format records, so an edit means exactly what a saved document
means.
"""

import pyclay as clay

import _render as R


def scene():
    """Three blobs whose ids we keep, so they can be edited afterwards."""
    doc = clay.Document()
    layer = doc.add_sdf_layer("body")
    ids = {
        "trunk": layer.add(clay.Capsule(a=(0, -0.8, 0), b=(0, 0.5, 0), r=0.45),
                           color="#5b8fb9"),
        "head": layer.add(clay.Sphere(r=0.5, position=(0, 1.0, 0)),
                          blend=clay.Smooth(0.25), color="#79bde0"),
        "wart": layer.add(clay.Sphere(r=0.3, position=(0.7, 0.2, 0.3)),
                          blend=clay.Smooth(0.15), color="#c85a4a"),
    }
    return doc, layer, ids


def main():
    R.banner("10 editing — changing a document that already exists")

    tiles = []

    doc, layer, ids = scene()
    tiles.append(R.render_tile(doc, layer=layer, size=200, colors_from_field=True))
    print(f"  placed 3 nodes: {ids}")

    # Move a placed node. Its id does not change, which is what lets a UI hold
    # a selection across the edit.
    doc, layer, ids = scene()
    layer.set_transform(ids["wart"], position=(-0.75, 0.5, 0.2))
    tiles.append(R.render_tile(doc, layer=layer, size=200, colors_from_field=True))

    # Swap a primitive. The node's blend and colour are its own and survive.
    doc, layer, ids = scene()
    layer.set_prim(ids["head"], clay.Box(size=(0.8, 0.8, 0.8)))
    tiles.append(R.render_tile(doc, layer=layer, size=200, colors_from_field=True))

    # Change how a node combines: the same wart, carved instead of added.
    doc, layer, ids = scene()
    layer.set_op_blend(ids["wart"], op=clay.Op.SUBTRACT, blend=clay.Smooth(0.2))
    tiles.append(R.render_tile(doc, layer=layer, size=200, colors_from_field=True))

    # Recolour in place — the field is untouched, only the colour changes.
    doc, layer, ids = scene()
    layer.set_color(ids["trunk"], "#7fb069")
    tiles.append(R.render_tile(doc, layer=layer, size=200, colors_from_field=True))

    # Remove a node; the others keep their ids and their place.
    doc, layer, ids = scene()
    layer.remove(ids["wart"])
    tiles.append(R.render_tile(doc, layer=layer, size=200, colors_from_field=True))

    R.contact_sheet(tiles, "10_editing.png", columns=3,
                    caption="original, moved, primitive swapped, op changed, "
                            "recoloured, removed")

    # Layers edit too: hiding one is exact and reversible.
    doc, layer, ids = scene()
    probes = __import__("numpy").random.default_rng(4).uniform(
        -2, 2, size=(512, 3)).astype("float32")
    before = doc.eval(probes)
    doc.set_layer_visible(layer.id, False)
    doc.set_layer_visible(layer.id, True)
    identical = (before == doc.eval(probes)).all()
    print(f"  hiding and showing a layer restores the field exactly: {identical}")
    if not identical:
        raise SystemExit("layer visibility was not reversible")

    # A stroke can be extended after it is placed — what a drag gesture issues.
    doc = clay.Document()
    layer = doc.add_sdf_layer("l")
    node = layer.add(clay.Stroke(points=[(-1.0, 0, 0, 0.25)], blend_k=0.05),
                     color="#e8b04b")
    for i in range(1, 9):
        x = -1.0 + i * 0.25
        layer.append_stroke(node, [(x, 0.35 * (i % 3 - 1), 0.0, 0.25)])
    eye, target = R.layer_camera(layer)
    R.render(doc, "10_stroke_extended.png", eye=eye, target=target,
             colors_from_field=True, caption="a stroke grown point by point")
    R.export_model(doc, "10_edited.ply", resolution=64)


if __name__ == "__main__":
    main()
