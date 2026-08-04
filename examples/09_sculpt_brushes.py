"""Falloff brushes and the sculpting verbs.

Voxel occupancy is binary, so a soft brush cannot make a cell half-solid.
What it can do is cover a *fraction* of its footprint, thinning toward the
rim — which is what a falloff curve plus a deterministic dither gives. The
dither is a hash of the cell coordinate, not a random number, so the same
stroke always produces the same cells and these renders are reproducible.

The four verbs reshape material that is already there, rather than stamping
a footprint into it.
"""

import pyclay as clay

import _render as R

VOXEL_SIZE = 0.1


def slab(thickness=2):
    """A flat slab: the substrate the verbs act on."""
    grid = clay.VoxelGrid(voxel_size=VOXEL_SIZE)
    stone = grid.palette_add("#8d97a4")
    grid.fill_box((-9, -thickness, -9), (9, 0, 9), stone)
    return grid, stone


def bumpy_slab():
    """A slab with a ridge and a pit, so flatten and smooth have work to do."""
    grid, stone = slab()
    accent = grid.palette_add("#c07a52")
    for x in range(-6, 7):
        grid.set((x, 1, -3), accent)
        grid.set((x, 2, -3), accent)          # a thin ridge
    grid.set_brush((3, 1, 4), 5, accent, shape="sphere")   # a lump
    grid.erase_brush((-4, 0, 3), 5, shape="sphere")        # a pit
    return grid, stone


def main():
    R.banner("09 sculpt brushes — falloff and the four verbs")

    # --- falloff curves ---------------------------------------------------
    tiles = []
    for falloff in ("constant", "linear", "smooth", "gaussian"):
        grid = clay.VoxelGrid(voxel_size=VOXEL_SIZE)
        i = grid.palette_add("#5f93bf")
        grid.set_brush((0, 0, 0), 17, i, shape="sphere", falloff=falloff)
        print(f"  falloff {falloff:9s} -> {grid.occupied_count:5d} cells")
        eye, target = R.voxel_camera(grid, VOXEL_SIZE, elevation=18.0)
        tiles.append(R.render_voxels_array(grid, eye=eye, target=target,
                                           width=190, height=190))
    R.contact_sheet(tiles, "09_falloff_curves.png", columns=4,
                    caption="constant, linear, smooth, gaussian")

    # --- strength ----------------------------------------------------------
    tiles = []
    for strength in (1.0, 0.7, 0.4, 0.15):
        grid = clay.VoxelGrid(voxel_size=VOXEL_SIZE)
        i = grid.palette_add("#c85a4a")
        grid.set_brush((0, 0, 0), 17, i, shape="sphere",
                       falloff="smooth", strength=strength)
        print(f"  strength {strength:<4} -> {grid.occupied_count:5d} cells")
        eye, target = R.voxel_camera(grid, VOXEL_SIZE, elevation=18.0)
        tiles.append(R.render_voxels_array(grid, eye=eye, target=target,
                                           width=190, height=190))
    R.contact_sheet(tiles, "09_falloff_strength.png", columns=4,
                    caption="the same brush at strength 1.0, 0.7, 0.4, 0.15")

    # --- the four verbs ----------------------------------------------------
    # Every tile starts from the same bumpy slab, so the only difference is
    # which verb ran.
    verbs = []

    base, _ = bumpy_slab()
    verbs.append(("untouched", base))

    g, _ = bumpy_slab()
    g.sculpt_smooth((0, 0, 0), 15)
    verbs.append(("smooth", g))

    g, _ = bumpy_slab()
    g.sculpt_inflate((0, 0, 0), 15, amount=1)
    verbs.append(("inflate", g))

    g, _ = bumpy_slab()
    g.sculpt_inflate((0, 0, 0), 15, amount=-1)
    verbs.append(("erode", g))

    g, _ = bumpy_slab()
    g.sculpt_flatten((0, 0, 0), 15, normal=(0, 1, 0))
    verbs.append(("flatten", g))

    g, _ = bumpy_slab()
    g.sculpt_pinch((0, 0, 0), 13)
    verbs.append(("pinch", g))

    tiles = []
    for name, grid in verbs:
        print(f"  {name:10s} {grid.occupied_count:5d} cells")
        eye, target = R.voxel_camera(grid, VOXEL_SIZE, azimuth=30.0, elevation=26.0)
        tiles.append(R.render_voxels_array(grid, eye=eye, target=target,
                                           width=210, height=190))
    R.contact_sheet(tiles, "09_sculpt_verbs.png", columns=3,
                    caption=", ".join(n for n, _ in verbs))

    # --- determinism -------------------------------------------------------
    a = clay.VoxelGrid(voxel_size=VOXEL_SIZE)
    b = clay.VoxelGrid(voxel_size=VOXEL_SIZE)
    ia, ib = a.palette_add("#ffffff"), b.palette_add("#ffffff")
    a.set_brush((0, 0, 0), 13, ia, shape="sphere", falloff="linear", seed=7)
    b.set_brush((0, 0, 0), 13, ib, shape="sphere", falloff="linear", seed=7)
    same = all(
        a.get((x, y, z)) == b.get((x, y, z))
        for x in range(-7, 8) for y in range(-7, 8) for z in range(-7, 8)
    )
    print(f"  same seed reproduces the same {a.occupied_count} cells: {same}")
    if not same:
        raise SystemExit("falloff dither is not deterministic")

    # A soft brush is still a brush: mesh and export one.
    R.save_model(verbs[1][1].mesh(), "09_smoothed.ply")


if __name__ == "__main__":
    main()
