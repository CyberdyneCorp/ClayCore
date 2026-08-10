"""Carrying a mesh, as opposed to sampling one.

`19_mesh_import` covers one reason to import a model: you want to *sculpt* it,
so the triangles become a distance field and stop being triangles. This is the
other reason, and it is the opposite requirement. A scan, a scale reference, a
kit part, a piece of hard surface authored elsewhere — geometry that has to
leave the pipeline as the same triangles it entered with.

Sampling cannot do that. A `Volume.from_mesh` is a resampled approximation on a
lattice somebody picked, with the uvs and the original vertex order gone; a
document holding one cannot re-export what was imported. So a **mesh layer**
stores the triangles verbatim and never evaluates them.

That "never" is the whole design, and it is structural rather than a promise.
The geometry is held beside the document, keyed by layer id, where voxel grids
and mask fields already live — the module layering forbids `clay::scene` from
seeing mesh data at all, so a mesh layer *cannot* enter a tape, a blend, an
influence bound or a pick. The consequence is visible in the renders below: the
document's own field shows the sculpt and nothing else. To see the carried
triangles in the same frame, something has to resample them, and this example
does that deliberately, for display only.

What a mesh layer *does* have is everything a layer has: a name, an order, a
transform, visibility, ghost and lock. Moving one moves what gets exported, not
what is stored.
"""

import os

import numpy as np

import pyclay as clay

import _render as R


def sculpt():
    """The thing being worked on: an ordinary SDF layer."""
    doc = clay.Document()
    layer = doc.add_sdf_layer("body")
    layer.add(clay.Sphere(r=0.46), color="#b0784a")
    layer.add(clay.Capsule(a=(0, 0.3, 0), b=(0, 0.85, 0), r=0.17),
              blend=clay.Smooth(0.12), color="#b0784a")
    return doc, layer


def reference_model(path):
    """Write a model to disk and import it back, so what follows is a real
    import through a real file rather than a mesh handed over in memory."""
    doc = clay.Document()
    layer = doc.add_sdf_layer("ref")
    layer.add(clay.Box(size=(0.34, 0.34, 0.34), position=(0, 0, 0)), rounding=0.05)
    layer.add(clay.Torus(R=0.4, r=0.09, rotation_axis_angle=((1, 0, 0), 1.5708)),
              blend=clay.Smooth(0.05))
    # Small on purpose: the re-export below is committed, and the point of this
    # example is that it comes back unchanged, not that it is dense.
    doc.mesh(resolution=48, decimate=0.22).save(path)
    return clay.load_mesh(path)


def preview_doc(mesh, cell=0.02, colour="#7f8a94", **placement):
    """A DISPLAY-ONLY document for a carried mesh.

    A mesh layer is not evaluated, so the renderer — which raycasts a field —
    has nothing to trace against. Resampling it here is exactly the
    approximation a mesh layer exists to avoid; it is fine for a picture and
    would be wrong for the export.
    """
    doc = clay.Document()
    doc.add_sdf_layer("preview").add(
        clay.Volume.from_mesh(mesh, cell=cell, **placement), color=colour)
    return doc


def main():
    R.banner("36 mesh layers — a document that CARRIES triangles")

    EYE, TARGET = (2.9, 1.7, 3.1), (0.25, 0.25, 0.0)
    # Where the reference model sits once its layer is moved: beside the
    # sculpt, so both read in one frame.
    PLACED = (0.8, 0.05, 0.0)
    obj_path = R.output_path("36_reference.obj")
    doc_path = R.output_path("36_mesh_layers.clayspace")

    # --- import, then carry ---------------------------------------------------
    imported = reference_model(obj_path)
    print(f"  imported {os.path.basename(obj_path)}: "
          f"{imported.triangle_count} triangles, bounds {imported.bounds[1]}")

    doc, _ = sculpt()
    carried = doc.add_mesh_layer(imported, "reference", scale=0.85)
    print(f"  carried it as a mesh layer: {carried.triangle_count} triangles, "
          f"{len(carried.positions)} vertices")

    # The buffers are the document's own memory, not a copy per read.
    assert np.asarray(carried.positions).base is not None

    # --- it changes nothing about the field ----------------------------------
    # The same document without the mesh layer must evaluate identically. Not
    # "close": identically, because the mesh never reaches the tape at all.
    plain, _ = sculpt()
    rng = np.random.default_rng(36)
    probes = rng.uniform(-1.2, 1.2, size=(4000, 3)).astype(np.float32)
    if not np.array_equal(doc.eval(probes), plain.eval(probes)):
        raise SystemExit("a mesh layer changed what the document evaluates to")
    print(f"  over {len(probes)} probes the field is bit-identical with and "
          f"without the mesh layer")

    # --- save, reload, compare element for element ---------------------------
    doc.save(doc_path)
    size = os.path.getsize(doc_path)
    back = clay.load(doc_path)
    restored = back.mesh_layer("reference")
    same = (np.array_equal(np.asarray(restored.positions), np.asarray(carried.positions))
            and np.array_equal(np.asarray(restored.indices), np.asarray(carried.indices))
            and np.array_equal(np.asarray(restored.normals), np.asarray(carried.normals)))
    print(f"  saved {size // 1024} KiB, reloaded: geometry identical {same}")
    if not same:
        raise SystemExit("the carried mesh did not survive the round trip")

    # The source file is not consulted on load — the triangles are IN the
    # document. Deleting it proves it rather than asserting it.
    os.remove(obj_path)
    os.remove(obj_path[:-4] + ".mtl")  # the OBJ writer's companion
    again = clay.load(doc_path)
    if again.mesh_layer("reference").triangle_count != carried.triangle_count:
        raise SystemExit("the reload depended on the file it was imported from")
    print("  deleted the source .obj and reloaded again: unaffected")

    # --- the transform moves the export, not the stored vertices -------------
    before = np.asarray(carried.positions).copy()
    doc.set_layer_transform(carried.layer, position=PLACED)
    if not np.array_equal(np.asarray(carried.positions), before):
        raise SystemExit("moving the layer moved the stored geometry")
    print("  moved the layer: the stored vertices did not move")

    # --- what each one actually is -------------------------------------------
    tiles = [
        R.render_array(doc, eye=EYE, target=TARGET, width=205, height=195),
        R.render_array(preview_doc(restored), eye=EYE, target=TARGET,
                       width=205, height=195),
        R.render_array(preview_doc(restored, position=PLACED),
                       eye=EYE, target=TARGET, width=205, height=195),
    ]
    R.contact_sheet(tiles, "36_mesh_layers_parts.png", columns=3,
                    caption="the document's field (the mesh is not in it), the carried "
                            "triangles resampled for display, and the same under the "
                            "layer transform")

    # --- both in one frame ---------------------------------------------------
    # The sculpt and the carried reference, drawn together. The merge happens
    # here in the example rather than in the document, because folding
    # triangles into the field is the very thing a mesh layer refuses to do.
    together = clay.Document()
    scene = together.add_sdf_layer("scene")
    scene.add(clay.Sphere(r=0.46), color="#b0784a")
    scene.add(clay.Capsule(a=(0, 0.3, 0), b=(0, 0.85, 0), r=0.17),
              blend=clay.Smooth(0.12), color="#b0784a")
    scene.add(clay.Volume.from_mesh(restored, cell=0.02, position=PLACED),
              color="#7f8a94")
    R.render(together, "36_mesh_layers.png", eye=EYE, target=TARGET,
             colors_from_field=True,
             caption="a sculpt and the model it was measured against, carried in the "
                     "same document and reloaded from it")

    # --- re-export what was imported -----------------------------------------
    # The point of the whole exercise: the triangles that come out are the
    # triangles that went in.
    R.save_model(restored, "36_reference_reexported.ply")
    os.remove(doc_path)


if __name__ == "__main__":
    main()
