"""Block out coarse, subdivide, refine — and what each level costs.

A grid holds a stack of resolution levels. Level 0 is the coarsest, level k has
half the cell size of level k-1, and one level is *active*: every verb acts on
it. A grid that never gains a second level is exactly the grid that existed
before this, which is why nothing in the other thirty-five examples changed.

Three things here are worth reading rather than only looking at.

**Levels, not an octree.** The alternative was one adaptive structure refined
only where the surface needs it, and it was rejected for a specific reason: a
falloff brush resolves sub-unit strength by hashing the *cell coordinate*, and
that hash is what makes a stroke land on the same cells on every platform and
backend. A stack of uniform lattices keeps that property untouched — each level
is a lattice with its own integer cells. An adaptive structure changes what a
cell coordinate is, and puts the guarantee the parity suite enforces in
question.

**Adding a level cannot move the surface.** Subdivision splits every occupied
cell into its eight children, so the solid is the same solid. The cost is
memory, and this example prints it: eight times the cells per level, which is
the honest reason to add one only when the detail needs it.

**Detail survives a coarse edit, which is the whole point.** Editing at a level
averages down into the coarser ones and replays into the finer ones from the
offsets they hold. So a broad stroke made at level 0 arrives at level 2 without
flattening the millimetre work already there — the loop sculpting is actually
made of. Without that, stepping down a level would be destructive and the
workflow would not exist.
"""

import pyclay as clay

import _render as R

BASE_VOXEL = 0.08


def blockout():
    """A crude two-box form, at the coarsest level a sculptor would tolerate."""
    g = clay.VoxelGrid(voxel_size=BASE_VOXEL)
    shell = g.palette_add("#8d99ae")
    g.fill_box((-6, -4, -4), (6, 2, 4), shell)      # body
    g.fill_box((-3, 3, -3), (3, 6, 3), shell)       # dome
    return g


def tile(grid, level, **kwargs):
    """Render one level. Every query the renderer makes — raycast, get, bounds —
    already acts on the active level, so selecting one is the whole change."""
    grid.active_level = level
    eye, target = R.voxel_camera(grid, grid.voxel_size, **kwargs)
    return R.render_voxels_array(grid, eye=eye, target=target, width=250, height=230)


def report(grid):
    print(f"  {'level':<7}{'cell size':>12}{'cells':>12}{'vs level 0':>13}")
    base = grid.level_occupied_count(0)
    for level in range(grid.level_count):
        cells = grid.level_occupied_count(level)
        print(f"  {level:<7}{grid.level_voxel_size(level):>12.4f}{cells:>12}"
              f"{cells / max(base, 1):>12.1f}x")


def main():
    R.banner("39 multi-resolution")

    g = blockout()
    trim = g.palette_add("#ef476f")
    print(f"  blocked out at {g.voxel_size:.3f} with one level, "
          f"{g.occupied_count} cells")

    # --- subdivide twice -----------------------------------------------------
    coarse_cells = g.occupied_count
    assert g.add_level() == 1
    assert g.add_level() == 2
    print("\n  after subdividing twice:")
    report(g)
    if g.level_occupied_count(2) != coarse_cells * 64:
        raise SystemExit("subdivision is meant to be exact, not a resample")

    # --- detail only where it is needed --------------------------------------
    # Ribs proud of the dome, at the finest level. Each is one fine cell — a
    # quarter of a coarse one — so it is not representable at level 0 at all,
    # and averaging down leaves it behind: one occupied child in eight is not a
    # majority, so levels 0 and 1 never see it.
    g.active_level = 2
    for x in range(-12, 16, 4):
        g.fill_box((x, 28, -12), (x, 28, 15), trim)
    print(f"\n  ribs added at level 2: {g.occupied_count - coarse_cells * 64} cells, "
          f"each {g.voxel_size:.4f} across")
    if g.level_occupied_count(0) != coarse_cells:
        raise SystemExit("sub-coarse detail is not meant to show up at level 0")

    # --- a broad stroke at the coarse level ----------------------------------
    # Pushing the big form around is cheap here and would be 64x the writes at
    # level 2 — which is the reason to have levels at all.
    g.active_level = 0
    before = g.change_count
    g.fill_box((-6, -6, -4), (6, -5, 4), g.palette_add("#8d99ae"))  # a plinth
    print(f"  plinth blocked in at level 0: {g.change_count - before} cell writes")

    g.active_level = 2
    plinth_at_fine = g.get((-20, -20, 0))
    ribs_still_there = g.get((-12, 28, 0)) == trim
    print(f"  back at level 2: plinth present={plinth_at_fine != 0}, "
          f"ribs intact={ribs_still_there}")
    if plinth_at_fine == 0 or not ribs_still_there:
        raise SystemExit("a coarse edit must arrive fine without flattening detail")

    print("\n  final cost per level:")
    report(g)

    R.contact_sheet([tile(g, 0, azimuth=35.0, elevation=22.0),
                     tile(g, 1, azimuth=35.0, elevation=22.0),
                     tile(g, 2, azimuth=35.0, elevation=22.0)],
                    "39_levels.png", columns=3,
                    caption="the same sculpt at level 0, 1 and 2")

    g.active_level = 2
    R.save_model(g.mesh(), "39_multi_resolution.ply")


if __name__ == "__main__":
    main()
