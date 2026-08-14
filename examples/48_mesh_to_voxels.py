"""An imported model, straight to voxels — one sampling instead of two.

`19_mesh_import` takes a triangle mesh into a *field*: `Volume.from_mesh` reads
the BVH and the generalized winding number and hands back a narrow band. That
has always been one step. Reaching the *voxel verbs* was four, and paid for two
samplings on the way:

    mesh = clay.load_mesh("model.obj")
    vol  = clay.Volume.from_mesh(mesh, cell=0.02)   # sampling 1: triangles -> band
    doc  = clay.Document()                          # a document only to throw away
    doc.add_sdf_layer("import").add(vol)
    grid.rasterize(doc, region)                     # sampling 2: band -> cells

Each sampling places the surface within about half a cell of its own lattice, so
the second one quantises a field that was already quantised — and a feature that
survived the first can fall between centres on the second. `rasterize_mesh` asks
the triangles directly:

    grid.rasterize_mesh(mesh)                       # one sampling, no document

The sign is the same one `19_mesh_import` exists to defend: **the generalized
winding number**, not a parity ray cast and not the nearest triangle's normal.
Both of those are exact on a clean closed mesh and wrong on the meshes people
actually import — one hole flips a parity count for a whole half-space. This
script rasterizes a holed model on purpose to show it does not.

There is a second difference the counts do not show and the render does:
**colour survives one sampling and not two.** `Volume.from_mesh` samples a
distance field and carries no colour at all, so the detour reaches the grid with
nothing to quantise and takes a default; `rasterize_mesh` reads the mesh's own
vertex colours and quantises them to the palette.

What sampling costs is unchanged and is stated rather than discovered: the
surface moves by up to half a cell, a feature thinner than a cell can vanish,
a sharp edge staircases, and colours closer than the palette tolerance merge.
"""

import os

import numpy as np

import pyclay as clay

import _render as R

CELL = 0.022


def source_model(path):
    """Write a model to disk and import it back, so what follows is a real
    import through a real file rather than a mesh handed over in memory."""
    doc = clay.Document()
    layer = doc.add_sdf_layer("source")
    layer.add(clay.Sphere(r=0.42), color="#c05a3c")
    layer.add(clay.Capsule(a=(0, 0.2, 0), b=(0, 0.72, 0), r=0.16),
              blend=clay.Smooth(0.10), color="#c05a3c")
    layer.add(clay.Torus(R=0.40, r=0.075, position=(0, -0.20, 0)),
              blend=clay.Smooth(0.05), color="#c05a3c")
    doc.mesh(resolution=96).save(path)
    return clay.load_mesh(path)


def drop_a_cap(mesh, above=0.55):
    """Delete the triangles above a height: a model with a hole in it, which is
    what an imported scan or a badly exported asset actually looks like."""
    positions = np.array(mesh.positions, copy=True)
    faces = np.array(mesh.indices, copy=True)
    keep = ~(positions[faces][:, :, 1] > above).all(axis=1)
    return clay.Mesh.from_triangles(positions, faces[keep])


def via_the_detour(mesh, cell, region):
    """The four-step chain this example replaces, kept so the comparison is
    against the real thing rather than against a description of it."""
    doc = clay.Document()
    doc.add_sdf_layer("import").add(clay.Volume.from_mesh(mesh, cell=cell))
    grid = clay.VoxelGrid(cell)
    grid.rasterize(doc, region=region)
    return grid


def main():
    R.banner("48 mesh to voxels — one sampling, not two")

    obj_path = R.output_path("48_source.obj")
    model = source_model(obj_path)
    print(f"  imported {os.path.basename(obj_path)}: {model.triangle_count} triangles")

    # --- one sampling ---------------------------------------------------------
    direct = clay.VoxelGrid(CELL)
    direct.rasterize_mesh(model)          # no region: a mesh has its own bounds
    print(f"  rasterize_mesh:      {direct.occupied_count:>6} cells, "
          f"{direct.palette_size} palette entries")

    # --- the detour it replaces -----------------------------------------------
    lo, hi = model.bounds
    pad = 4 * CELL
    region = (tuple(np.array(lo) - pad), tuple(np.array(hi) + pad))
    detoured = via_the_detour(model, CELL, region)
    print(f"  the 4-step detour:   {detoured.occupied_count:>6} cells, "
          f"{detoured.palette_size} palette entries")
    agreement = 1.0 - abs(direct.occupied_count - detoured.occupied_count) / max(
        direct.occupied_count, detoured.occupied_count)
    print(f"  the two agree on the solid to {agreement * 100:.1f}% of its cells — "
          f"they differ at the surface, which is where a sampling lives")

    # ...and they do NOT agree about colour, which is the difference you can see
    # in the render rather than count. `Volume.from_mesh` samples a DISTANCE
    # field and carries no colour, so the detour arrives at the grid with
    # nothing to quantise and takes the document's default. One sampling reads
    # the mesh's own vertex colours.
    def entries(grid):
        return [tuple(round(c, 3) for c in grid.palette_color(i))
                for i in range(1, grid.palette_size)]
    print(f"  colour   direct {entries(direct)}   detour {entries(detoured)}")
    if entries(direct) == entries(detoured):
        raise SystemExit("the detour kept the model's colour, so this claim is stale")

    eye, target = R.voxel_camera(direct, CELL, azimuth=32.0, elevation=20.0)
    R.side_by_side(
        R.render_voxels_array(direct, eye=eye, target=target),
        R.render_voxels_array(detoured, eye=eye, target=target),
        "48_one_sampling_vs_two.png",
        caption="rasterize_mesh (left) and the band-then-rasterize detour")

    # --- a thin feature is where the difference stops being cosmetic ----------
    # A fin thinner than two cells: the detour samples it into a band and then
    # samples the band, and it can fall between centres on the second pass.
    fin = thin_fin(half_thickness=CELL * 0.62)
    fin_region = ((-0.5, -0.5, -0.5), (0.5, 0.5, 0.5))
    fin_direct = clay.VoxelGrid(CELL)
    fin_direct.rasterize_mesh(fin, region=fin_region)
    fin_detour = via_the_detour(fin, CELL, fin_region)
    print(f"\n  a fin {2 * CELL * 0.62:.3f} thick, against a {CELL} cell:")
    print(f"    rasterize_mesh:    {fin_direct.occupied_count:>6} cells")
    print(f"    the 4-step detour: {fin_detour.occupied_count:>6} cells")
    if fin_direct.occupied_count < fin_detour.occupied_count:
        raise SystemExit("the detour kept more of the fin than one sampling did")

    # --- dirty input is the input --------------------------------------------
    holed = drop_a_cap(model)
    print(f"\n  dropped the cap: {model.triangle_count} -> {holed.triangle_count} triangles")
    holed_grid = clay.VoxelGrid(CELL)
    holed_grid.rasterize_mesh(holed)
    ratio = holed_grid.occupied_count / direct.occupied_count
    print(f"  rasterized: {holed_grid.occupied_count} cells, {ratio * 100:.0f}% of the "
          f"closed model — the hole costs the volume above it and nothing else")
    if not 0.5 < ratio < 1.05:
        raise SystemExit("the hole flipped a half-space")

    R.render_voxels(holed_grid, "48_holed.png", eye=eye, target=target,
                    caption="a model missing its cap: the winding number degrades, "
                            "it does not invert")

    # --- and now it is voxels, so the verbs apply ----------------------------
    # The point of the trip: an imported model reaching the ten sculpting verbs
    # without a document in the middle.
    sculpted = clay.VoxelGrid(CELL)
    sculpted.rasterize_mesh(model)
    before = sculpted.change_count

    # Aim the brushes the way a host does — by raycasting the grid — rather than
    # by typing cell coordinates that happen to be inside the solid, where a
    # majority filter has nothing to decide and reports a legal no-op.
    def surface_cell(origin, direction):
        hit = sculpted.raycast(origin, direction)
        if hit is None:
            raise SystemExit(f"the probe ray from {origin} missed the imported solid")
        return hit["cell"]

    # Firm and repeated, because a picture is part of the claim: a dab that
    # moves thirty cells out of forty thousand is a real edit and an invisible
    # render, and a render nobody can read is not evidence.
    tip = surface_cell((0.0, 2.0, 0.0), (0.0, -1.0, 0.0))
    ring = surface_cell((2.0, -0.20, 0.0), (-1.0, 0.0, 0.0))
    for _ in range(3):
        sculpted.sculpt_inflate(tip, size=34, amount=2, shape="sphere",
                                falloff="smooth", strength=1.0)
        sculpted.sculpt_smooth(ring, size=34, shape="sphere", falloff="smooth",
                               strength=1.0)
    print(f"\n  inflated the tip and smoothed the ring, aiming by raycast: "
          f"{sculpted.change_count - before} cells changed")
    if sculpted.change_count == before:
        raise SystemExit("the voxel verbs did nothing to the imported model")

    R.side_by_side(
        R.render_voxels_array(direct, eye=eye, target=target),
        R.render_voxels_array(sculpted, eye=eye, target=target),
        "48_sculpted.png",
        caption="the imported model as cells, then inflated and smoothed by the "
                "voxel verbs — no document in the middle")

    os.remove(obj_path)
    companion = obj_path[:-4] + ".mtl"
    if os.path.exists(companion):
        os.remove(companion)


def thin_fin(half_thickness):
    """A flat slab thinner than two cells: twelve triangles, wound outward."""
    lo = (-0.30, -half_thickness, -0.30)
    hi = (0.30, half_thickness, 0.30)
    p = np.array([[lo[0], lo[1], lo[2]], [hi[0], lo[1], lo[2]], [hi[0], hi[1], lo[2]],
                  [lo[0], hi[1], lo[2]], [lo[0], lo[1], hi[2]], [hi[0], lo[1], hi[2]],
                  [hi[0], hi[1], hi[2]], [lo[0], hi[1], hi[2]]], dtype=np.float32)
    f = np.array([0, 3, 2, 0, 2, 1, 4, 5, 6, 4, 6, 7,
                  0, 1, 5, 0, 5, 4, 3, 7, 6, 3, 6, 2,
                  0, 4, 7, 0, 7, 3, 1, 2, 6, 1, 6, 5], dtype=np.uint32)
    return clay.Mesh.from_triangles(p, f)


if __name__ == "__main__":
    main()
