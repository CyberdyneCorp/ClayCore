"""Displaying a voxel sculpt as a form rather than as boxes (#108).

The voxel verbs were never the problem. `mesh()` is a GREEDY mesher — merged
axis-aligned quads — so a sculpt made of the right cells is shown as the wrong
shape, and every voxel render in this gallery looked like Minecraft while every
SDF render looked like clay.

`mesh_smooth()` is surface nets over the same occupancy, sampled at voxel
centres: one vertex per surface cell, at the centroid of that cell's edge
crossings. The centroid is what smooths — a corner cell's vertex is pulled
toward the average of its crossings — so nothing has to be filtered first, and
nothing VANISHES: a lone voxel has a sign change on each of its six edges.

Both meshes describe the same cells. Which one a host wants depends on the
sculpt: hard-surface voxel work and export want the boxes, an organic form
wants the round.

Rendered the way 36_mesh_layers renders a mesh — through `Volume.from_mesh`,
because the renderer raycasts a field rather than triangles.
"""

import numpy as np

import pyclay as clay

import _render as R

CELL = 0.04
EYE = (2.4, 1.8, 2.9)
TARGET = (0.0, 0.1, 0.0)


def sculpt():
    """A blobby form a sculptor would recognise: a body, a head, two arms."""
    g = clay.VoxelGrid(CELL)
    skin = g.palette_add((0.85, 0.62, 0.48))
    coat = g.palette_add((0.30, 0.45, 0.70))

    # Body and head as spheres, arms as swept stamps — all through the ordinary
    # brush, so this is a sculpt rather than a rasterized primitive.
    g.set_brush((0, 0, 0), 22, coat, shape="sphere")
    g.set_brush((0, 16, 0), 13, skin, shape="sphere")
    for i in range(14):
        t = i / 13.0
        x = int(round(10 + 12 * t))
        y = int(round(6 + 6 * t))
        g.set_brush((x, y, 0), 7, coat, shape="sphere")
        g.set_brush((-x, y, 0), 7, coat, shape="sphere")
    # A couple of dents, so there is something for the smoothing to round.
    g.sculpt_smooth((0, 6, 11), size=9)
    g.sculpt_smooth((0, 14, 9), size=7)
    return g


def preview(mesh, colour):
    """A document that shows a MESH: the renderer raycasts a field, so the
    mesh is resampled into a volume exactly as 36_mesh_layers does."""
    doc = clay.Document()
    layer = doc.add_sdf_layer("preview")
    layer.add(clay.Volume.from_mesh(mesh, cell=CELL * 0.75), color=colour)
    return doc


def main():
    g = sculpt()
    print(f"  the sculpt: {g.occupied_count} occupied cells at {CELL} cell size")

    blocky = g.mesh()
    smooth = g.mesh_smooth()
    smoother = g.mesh_smooth(blur=1)

    print(f"  mesh()             {blocky.triangle_count:6d} triangles  (merged quads)")
    print(f"  mesh_smooth()      {smooth.triangle_count:6d} triangles  (one vertex per surface cell)")
    print(f"  mesh_smooth(blur=1){smoother.triangle_count:6d} triangles  (extra smoothing)")

    # The claim this example exists to show, as a number as well as a picture:
    # the smooth surface does not shrink the form, it rounds it. Both meshes
    # reach the same extent, because a crossing between an occupied and an
    # empty cell interpolates to the midpoint — which is the plane greedy
    # emits. What moves is the corners.
    def extent(m):
        p = np.asarray(m.positions, dtype=np.float32).reshape(-1, 3)
        return p.min(axis=0), p.max(axis=0)

    blo, bhi = extent(blocky)
    slo, shi = extent(smooth)
    print(f"  extent blocky {np.round(blo, 3)} .. {np.round(bhi, 3)}")
    print(f"  extent smooth {np.round(slo, 3)} .. {np.round(shi, 3)}")
    assert np.allclose(blo, slo, atol=CELL) and np.allclose(bhi, shi, atol=CELL), \
        "the smooth mesh should round the form, not shrink it"

    # And a lone voxel survives, which is why the default filters nothing.
    lone = clay.VoxelGrid(CELL)
    lone.set((0, 0, 0), lone.palette_add((1.0, 0.3, 0.3)))
    assert lone.mesh_smooth().triangle_count > 0, "a lone voxel must not vanish"
    assert lone.mesh_smooth(blur=1).triangle_count == 0, \
        "one blur pass is what erases it — that is why blur is not the default"
    print("  a lone voxel survives mesh_smooth(), and is erased by blur=1")

    R.save_model(blocky, "41_voxel_blocky.ply")
    R.save_model(smooth, "41_voxel_smooth.ply")

    tiles = [
        R.render_array(preview(blocky, "#c8792f"), eye=EYE, target=TARGET,
                       width=240, height=240),
        R.render_array(preview(smooth, "#c8792f"), eye=EYE, target=TARGET,
                       width=240, height=240),
        R.render_array(preview(smoother, "#c8792f"), eye=EYE, target=TARGET,
                       width=240, height=240),
    ]
    R.contact_sheet(tiles, "41_voxel_smooth_display.png", columns=3)
    print("  wrote output/41_voxel_smooth_display.png  "
          "(mesh() | mesh_smooth() | mesh_smooth(blur=1) — the same cells)")


if __name__ == "__main__":
    main()
