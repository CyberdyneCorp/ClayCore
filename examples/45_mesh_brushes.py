"""Sculpting the triangles themselves — the classical mode, on a mesh layer.

Every other brush in this gallery edits a FIELD. `12_strokes` appends items,
`15_voxel_verbs_and_repair` moves occupancy, `11_masks` freezes both. This one
moves the vertices of a mesh a document is *carrying*, and holds one line while
it does: **topology never changes.** No polygon is created, split or deleted;
the index buffer that goes in comes out byte for byte.

That sounds like a limitation and is the entire point. `36_mesh_layers` showed
why a document carries triangles verbatim — a scan, a kit part, a piece of
retopology somebody paid for. Until now the only way to *edit* one was
`Volume.from_mesh`, which resamples the model onto a lattice: the sculpt comes
back, the edge loops and the uvs do not. Fixed-topology brushes are the only
operation that touches a carried mesh without spending what makes it worth
carrying.

The six verbs here are the primitives — everything in `46` composes from them:

    grab      drag the vertices under the falloff
    draw      displace along the REGION's averaged normal — one direction
    inflate   displace along EACH VERTEX's own normal — signed
    smooth    Laplacian average over the one-ring
    pinch     signed: + gathers tangentially, - spreads (magnify)
    flatten   project toward a plane, in three modes

Draw and inflate look like the same brush with different words. They are not,
and the difference is measured below: draw picks ONE direction for the whole
stamp and makes a rounded organic swell; inflate gives every vertex its own and
puffs the surface along itself.

Stated because it is behaviour and not a defect: a large grab STRETCHES
triangles. That stretch is the signal the mesh wants retopo, exactly as Blender
behaves with Dyntopo off.
"""

import os

import numpy as np

import pyclay as clay

import _render as R

EYE, TARGET = (2.4, 1.9, 2.9), (0.0, 0.0, 0.0)


def source_shape(doc):
    layer = doc.add_sdf_layer("source")
    layer.add(clay.Sphere(r=0.62))
    layer.add(clay.Capsule(a=(0, 0.35, 0), b=(0, 0.95, 0), r=0.22),
              blend=clay.Smooth(0.14))
    layer.add(clay.Torus(R=0.55, r=0.10, position=(0, -0.35, 0)),
              blend=clay.Smooth(0.06))
    return layer


def source_model(path):
    """Write a model to disk and import it back, so what follows is a real
    import through a real file rather than a mesh handed over in memory."""
    doc = clay.Document()
    source_shape(doc)
    doc.mesh(resolution=96).save(path)
    return clay.load_mesh(path)


def preview(mesh, cell=0.014, colour="#b0784a"):
    """A DISPLAY-ONLY document for a carried mesh.

    A mesh layer is never evaluated, so the renderer — which raycasts a field —
    has nothing to trace against. Resampling here is exactly the approximation
    a mesh layer exists to avoid: fine for a picture, wrong for the export.
    """
    doc = clay.Document()
    doc.add_sdf_layer("preview").add(clay.Volume.from_mesh(mesh, cell=cell),
                                     color=colour)
    return doc


def copy_of(mesh):
    """An independent Mesh with the same vertices — the fixture each verb gets."""
    return clay.Mesh.from_triangles(np.array(mesh.positions, copy=True),
                                    np.array(mesh.indices, copy=True))


def direction_spread(base_positions, mesh):
    """Widest disagreement between two displacement directions, as 1 - dot.

    Near zero means every vertex moved the same way, which is what makes draw
    a swell; large means each went its own way, which is what inflate is.
    """
    d = np.array(mesh.positions) - base_positions
    d = d[np.linalg.norm(d, axis=1) > 1e-5]
    if len(d) == 0:
        return 0.0
    unit = d / np.linalg.norm(d, axis=1, keepdims=True)
    return float(1.0 - (unit @ unit[0]).min())


def main():
    R.banner("45 mesh brushes — moving vertices, never polygons")

    obj_path = R.output_path("45_source.obj")
    model = source_model(obj_path)
    base_positions = np.array(model.positions, copy=True)
    base_indices = np.array(model.indices, copy=True)
    print(f"  imported {os.path.basename(obj_path)}: {model.triangle_count} triangles, "
          f"{len(model.positions)} vertices")

    # The brush centre comes from a PICK, as it does in an app: the host has a
    # ray, and raycast() answers with the surface point and the walk seed. No
    # camera enters the engine.
    probe = clay.MeshSculptor(model)
    hit = probe.raycast(origin=(0.0, 2.0, 0.55), direction=(0.0, -1.0, 0.0))
    if hit is None:
        raise SystemExit("the probe ray missed the model")
    centre, seed = hit["position"], hit["seed_class"]
    print(f"  picked triangle {hit['triangle']} at "
          f"({centre[0]:+.3f}, {centre[1]:+.3f}, {centre[2]:+.3f}); walk seed {seed}")

    # Welded adjacency: fewer classes than vertices exactly where the import
    # split a position, which is how you can tell you have a seamed model.
    print(f"  adjacency: {probe.vertex_count} vertices -> {probe.class_count} welded "
          f"classes ({probe.vertex_count - probe.class_count} duplicates fused)")

    # --- the six primitives ---------------------------------------------------
    common = dict(center=centre, radius=0.42, seed_class=seed)
    verbs = [
        ("grab", dict(strength=1.0, direction=(0.22, 0.12, 0.0), **common)),
        ("draw", dict(strength=0.55, **common)),
        ("inflate", dict(strength=0.55, **common)),
        ("smooth", dict(strength=1.0, smooth_iterations=6, **common)),
        ("pinch", dict(strength=0.8, **common)),
        ("flatten", dict(strength=1.0, **common)),
    ]

    tiles = [R.render_tile(preview(model), eye=EYE, target=TARGET, size=180)]
    labels = []
    for verb, kwargs in verbs:
        work = copy_of(model)
        moved = clay.MeshSculptor(work).stamp(verb, **kwargs)
        same = np.array_equal(np.array(work.indices), base_indices)
        drift = float(np.abs(np.array(work.positions) - base_positions).max())
        print(f"  {verb:<8} moved {moved:>5} classes, furthest vertex {drift:.4f}, "
              f"topology identical: {same}")
        if not same:
            raise SystemExit(f"{verb} changed the topology")
        tiles.append(R.render_tile(preview(work), eye=EYE, target=TARGET, size=180))
        labels.append(verb)

    R.contact_sheet(tiles, "45_mesh_brushes_primitives.png", columns=4,
                    caption="untouched, then " + ", ".join(labels))

    # --- draw against inflate, which is where the difference lives ------------
    drawn, inflated = copy_of(model), copy_of(model)
    clay.MeshSculptor(drawn).stamp("draw", strength=0.9, **common)
    clay.MeshSculptor(inflated).stamp("inflate", strength=0.9, **common)
    R.side_by_side(
        R.render_array(preview(drawn), eye=EYE, target=TARGET, width=240, height=240),
        R.render_array(preview(inflated), eye=EYE, target=TARGET, width=240, height=240),
        "45_draw_vs_inflate.png",
        caption="draw's one shared direction (left) against inflate's per-vertex normals")
    print(f"  draw's displacements disagree by {direction_spread(base_positions, drawn):.5f}; "
          f"inflate's by {direction_spread(base_positions, inflated):.3f}")

    # --- flatten's three modes ------------------------------------------------
    # Cut-only is the hard-surface family — Trim Dynamic, hPolish, the Planar
    # brushes — where cutting WITHOUT filling is the whole brush.
    mode_tiles = []
    for mode in ("two_sided", "cut", "fill"):
        work = copy_of(model)
        moved = clay.MeshSculptor(work).stamp(
            "flatten", strength=1.0, flatten_mode=mode,
            plane_point=(centre[0], centre[1] - 0.06, centre[2]), plane_normal=(0, 1, 0),
            **common)
        print(f"  flatten {mode:<9} moved {moved:>5} classes")
        mode_tiles.append(R.render_tile(preview(work), eye=EYE, target=TARGET, size=180))
    R.contact_sheet(mode_tiles, "45_flatten_modes.png", columns=3,
                    caption="two_sided, cut (Trim Dynamic), fill")

    # --- the case the feature exists for: a quad mesh survives a stroke -------
    quad_doc = clay.Document()
    source_shape(quad_doc)
    quads = quad_doc.mesh_quads(cell_size=0.055)
    quads_before = np.array(quads.quads, copy=True)
    positions_before = np.array(quads.positions, copy=True)
    print(f"\n  quad-exported the same shape: {quads.quad_count} quads, "
          f"{quads.triangle_count} triangles")
    before_image = R.render_array(preview(quads, colour="#7f8a94"), eye=EYE, target=TARGET,
                                  width=240, height=240)

    sculptor = clay.MeshSculptor(quads)
    preset = clay.StrokePreset(radius=0.30, strength=0.8, spacing=0.3)
    samples = np.array([[-0.45 + 0.10 * i, 0.55, 0.35] for i in range(10)], dtype=np.float32)
    deltas = clay.VertexDeltas()
    applied = sculptor.apply_stroke(samples, preset, "draw", deltas=deltas)
    identical = np.array_equal(np.array(quads.quads), quads_before)
    print(f"  drew a {applied}-stamp stroke across it")
    print(f"  after: {quads.quad_count} quads, {quads.triangle_count} triangles, "
          f"quad list identical: {identical}")
    if not identical:
        raise SystemExit("the stroke changed the quads")

    R.side_by_side(
        before_image,
        R.render_array(preview(quads, colour="#7f8a94"), eye=EYE, target=TARGET,
                       width=240, height=240),
        "45_quads_survive.png",
        caption="a quad export before and after a stroke; the quad list is unchanged")

    # --- and it undoes exactly ------------------------------------------------
    deltas.revert(sculptor)
    exact = np.array_equal(np.array(quads.positions), positions_before)
    print(f"  reverted {deltas.vertex_count} recorded vertices; bit-identical to the "
          f"pre-stroke mesh: {exact}")
    if not exact:
        raise SystemExit("the vertex-delta revert was not exact")

    os.remove(obj_path)
    companion = obj_path[:-4] + ".mtl"  # the OBJ writer's companion
    if os.path.exists(companion):
        os.remove(companion)


if __name__ == "__main__":
    main()
