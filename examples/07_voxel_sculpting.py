"""Voxel sculpting: the editing surface, not just the data structure.

Covers the operations a sculpting UI actually issues — brush stamps, box and
line fills, mirrored strokes, flood select, palette edits — and finishes with
greedy meshing, which is what turns the grid into triangles.

Voxel renders go through `VoxelGrid.raycast`, which is one ray at a time, so
they run at lower resolution than the SDF examples.
"""

import numpy as np

import pyclay as clay

import _render as R

VOXEL_SIZE = 0.1


def new_grid():
    return clay.VoxelGrid(voxel_size=VOXEL_SIZE)


def main():
    R.banner("07 voxel sculpting — brushes, fills, selection, meshing")

    # --- palette + primitive edits -------------------------------------
    grid = new_grid()
    stone = grid.palette_add("#8d8b85")
    moss = grid.palette_add("#6b8e4e")
    ember = grid.palette_add("#d2691e")
    print(f"  palette has {grid.palette_size} entries")

    # A box fill for the base, a line for the spine, brush stamps for lumps.
    grid.fill_box((-8, -6, -8), (8, -4, 8), stone)
    grid.fill_line((-7, -3, 0), (7, 3, 0), moss)
    for cx in (-5, 0, 5):
        grid.set_brush((cx, -2, 3), 3, ember)

    eye, target = R.voxel_camera(grid, VOXEL_SIZE, elevation=28.0)
    R.render_voxels(grid, "07_voxel_edits.png", eye=eye, target=target,
                    caption="box fill, line fill, brush stamps")

    # --- mirrored sculpting --------------------------------------------
    grid = new_grid()
    body = grid.palette_add("#4f7fa8")
    trim = grid.palette_add("#e8c547")
    grid.fill_box((-2, -6, -3), (2, 4, 3), body)
    # Mirrored placement keeps the model symmetric while editing one side.
    for y in range(-4, 4):
        grid.set_mirrored((4, y, 1), trim, axes="x")
    for z in range(-2, 3):
        grid.set_mirrored((3, 5, z), trim, axes="x")

    eye, target = R.voxel_camera(grid, VOXEL_SIZE, elevation=22.0)
    R.render_voxels(grid, "07_voxel_mirror.png", eye=eye, target=target,
                    caption="set_mirrored keeps both sides in sync")

    # --- flood select + recolour ---------------------------------------
    grid = new_grid()
    a = grid.palette_add("#7a6f9b")
    b = grid.palette_add("#c96f6f")
    grid.fill_box((-7, -3, -3), (-2, 3, 3), a)
    grid.fill_box((2, -3, -3), (7, 3, 3), a)   # a second, disconnected blob
    selected = grid.flood_select((-4, 0, 0), same_color=True)
    print(f"  flood select from the left blob picked {len(selected)} cells")
    grid.set_many(selected, b)                  # recolour only what it reached

    eye, target = R.voxel_camera(grid, VOXEL_SIZE, elevation=20.0)
    R.render_voxels(grid, "07_voxel_flood.png", eye=eye, target=target,
                    caption="flood select reached one blob, not both")

    # --- erase + paint ---------------------------------------------------
    grid = new_grid()
    shell = grid.palette_add("#9aa8b5")
    inner = grid.palette_add("#d95f5f")
    grid.fill_box((-5, -5, -5), (5, 5, 5), shell)
    grid.erase_brush((0, 5, 0), 5)              # scoop a hollow from the top
    for cell in ((0, 0, 0), (1, 0, 0), (-1, 0, 0)):
        grid.paint(cell, inner)                 # recolour occupied cells only
    grid.erase((5, 5, 5))

    eye, target = R.voxel_camera(grid, VOXEL_SIZE, elevation=30.0)
    R.render_voxels(grid, "07_voxel_carve.png", eye=eye, target=target,
                    caption="erase_brush hollows, paint recolours in place")

    # --- greedy meshing ---------------------------------------------------
    mesh = grid.mesh()
    R.save_model(mesh, "07_voxel_greedy.ply")
    # Greedy meshing emits each merged quad with its own vertices, which is
    # what keeps per-face colour exact (no bleeding across a merge). Those
    # duplicated vertices mean the index-based watertightness check sees every
    # edge as a boundary, even though the surface is geometrically closed —
    # so a voxel mesh reports manifold but not watertight. The SDF meshers,
    # which share vertices, report watertight; see 08_meshing_and_io.py.
    print(f"  greedy mesh: manifold={mesh.is_manifold()}, "
          f"watertight={mesh.is_watertight()} (per-face vertices, see comment)")

    # --- picking ----------------------------------------------------------
    # What a tap in the viewport resolves to: the cell hit, and the empty
    # neighbour across the entry face, which is where a placement goes.
    hit = grid.raycast((2.4, 1.8, 3.0), (-0.62, -0.46, -0.77))
    if hit is not None:
        print(f"  ray hit cell {hit['cell']} on face {hit['face']}; "
              f"placement target {hit['adjacent']}")

    # --- step field --------------------------------------------------------
    # Distance-to-occupied, used to accelerate ray marching through emptiness.
    probes = np.array([[0.0, 0.0, 0.0], [2.0, 2.0, 2.0]], dtype=np.float32)
    print(f"  step field at probes: {grid.sample_step_field(probes)}")


if __name__ == "__main__":
    main()
