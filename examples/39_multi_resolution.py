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
DOME_CELLS = ((-3, 3, -3), (3, 6, 3))  # the dome blockout() fills
RIB_CELL = 0.02                        # level 2, where the ribs are authored
RIB_CELLS = ((-12, 28, -12), (15, 28, 15))  # the band they occupy


def blockout():
    """A crude two-box form, at the coarsest level a sculptor would tolerate."""
    g = clay.VoxelGrid(voxel_size=BASE_VOXEL)
    shell = g.palette_add("#8d99ae")
    g.fill_box((-6, -4, -4), (6, 2, 4), shell)      # body
    g.fill_box(*DOME_CELLS, shell)                  # dome
    return g


def rib_region():
    """The band the ribs live in, in world units — the motivating case exactly:
    detail that needs the finest cells over a few percent of the form.

    A region is rounded OUT to whole chunks (32 cells), so what it costs is
    measured in chunks rather than in the box you asked for. A band is cheap
    because it is thin in one axis; a box across the form's full extent would
    round out to most of its chunks and save almost nothing."""
    lo, hi = RIB_CELLS
    return (tuple(v * RIB_CELL for v in lo), tuple((v + 1) * RIB_CELL for v in hi))


def tile(grid, level, **kwargs):
    """Render one level. Every query the renderer makes — raycast, get, bounds —
    already acts on the active level, so selecting one is the whole change."""
    grid.active_level = level
    eye, target = R.voxel_camera(grid, grid.voxel_size, **kwargs)
    return R.render_voxels_array(grid, eye=eye, target=target, width=250, height=230)


KIB_PER_CHUNK = 32  # 32^3 cells, one byte each


def report(grid):
    """Cells are the SOLID; chunks are what it costs. On a wholly refined level
    the two track each other, and the point of a region is that they stop."""
    print(f"  {'level':<7}{'cell size':>12}{'cells':>12}{'vs L0':>8}"
          f"{'chunks':>9}{'stored':>10}")
    base = grid.level_occupied_count(0)
    for level in range(grid.level_count):
        cells = grid.level_occupied_count(level)
        chunks = grid.level_chunk_count(level)
        whole = "" if grid.level_is_whole(level) else "  (region)"
        print(f"  {level:<7}{grid.level_voxel_size(level):>12.4f}{cells:>12}"
              f"{cells / max(base, 1):>7.1f}x{chunks:>9}"
              f"{chunks * KIB_PER_CHUNK / 1024:>9.1f}M{whole}")


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

    # --- refine a REGION, not the whole lattice ------------------------------
    # Everything above pays 8x per level over the whole occupied volume, and
    # occupancy is volumetric: the cells above are the solid, not its surface.
    # A region is how a sculptor gets rib-resolution at the ribs without
    # storing the skull at rib resolution.
    #
    # Outside the region the level has NO storage and reads its parent's value.
    # So the lattice is still uniform and complete — the mesher sees no
    # boundary, `get` answers everywhere, bounds describe the whole solid — and
    # only what is STORED changes. That is why cells below are identical
    # between the two and chunks are not.
    print("\n  the same second level, whole vs over a region:")
    def with_ribs(grid):
        """The same ribs the sculpt above got, at the finest level — so the two
        panels show the DETAIL the region exists for rather than a bare
        blockout, and their being identical is the claim."""
        grid.active_level = 2
        trim_here = grid.palette_add("#ef476f")
        for x in range(-12, 16, 4):
            grid.fill_box((x, 28, -12), (x, 28, 15), trim_here)
        return grid

    whole = blockout()
    whole.add_level()
    whole.add_level()
    with_ribs(whole)

    regional = blockout()
    regional.add_level()
    regional.add_level_region(rib_region())
    with_ribs(regional)

    print(f"  {'':<24}{'cells':>12}{'chunks':>9}{'stored':>10}")
    for name, grid in (("whole lattice", whole), ("region (the ribs)", regional)):
        cells = grid.level_occupied_count(2)
        chunks = grid.level_chunk_count(2)
        print(f"  {name:<24}{cells:>12}{chunks:>9}"
              f"{chunks * KIB_PER_CHUNK / 1024:>9.1f}M")
    if regional.level_occupied_count(2) != whole.level_occupied_count(2):
        raise SystemExit("a region must change what is STORED, not what the solid is")
    if regional.level_chunk_count(2) >= whole.level_chunk_count(2):
        raise SystemExit("a region that stores as much as the whole lattice bought nothing")

    # And it is not a hole: a fine cell far outside the region still reads as
    # material, because it reads its parent.
    regional.active_level = 2
    whole.active_level = 2
    probe = (-20, -12, 0)  # in the BODY, far from the refined band
    if regional.get(probe) != whole.get(probe):
        raise SystemExit("outside the region a level must read its parent, not empty")
    print("  a cell far outside the region reads the same as on the whole lattice")

    # ...and the saving scales with how many CHUNKS the form spans, which is
    # the honest caveat: a chunk is 32 cells across, so a region is rounded out
    # to at least one, and the blockout above is only two chunks wide. No
    # render for this one — it is a number, and the picture would be the same
    # picture, which is exactly the claim.
    deep_whole, deep_region = blockout(), blockout()
    for _ in range(4):
        deep_whole.add_level()
    for _ in range(3):
        deep_region.add_level()
    deep_region.add_level_region(rib_region())
    wc, rc = deep_whole.level_chunk_count(4), deep_region.level_chunk_count(4)
    print(f"\n  four levels deep ({deep_whole.level_voxel_size(4):.4f} cells), where the form"
          f" spans many chunks:")
    print(f"  {'whole lattice':<24}{wc:>9} chunks{wc * KIB_PER_CHUNK / 1024:>9.1f}M")
    print(f"  {'region (the ribs)':<24}{rc:>9} chunks{rc * KIB_PER_CHUNK / 1024:>9.1f}M"
          f"   {wc / max(rc, 1):.0f}x less")
    if deep_region.level_occupied_count(4) != deep_whole.level_occupied_count(4):
        raise SystemExit("a region must change what is STORED, not what the solid is")
    if rc * 3 > wc:
        raise SystemExit("the saving is meant to grow with the chunks the form spans")

    R.contact_sheet([tile(whole, 2, azimuth=35.0, elevation=22.0),
                     tile(regional, 2, azimuth=35.0, elevation=22.0)],
                    "39_regional.png", columns=2,
                    caption="level 2 refined wholly, and over the rib band only — "
                            "the same solid, a fraction of the storage")

    R.contact_sheet([tile(g, 0, azimuth=35.0, elevation=22.0),
                     tile(g, 1, azimuth=35.0, elevation=22.0),
                     tile(g, 2, azimuth=35.0, elevation=22.0)],
                    "39_levels.png", columns=3,
                    caption="the same sculpt at level 0, 1 and 2")

    g.active_level = 2
    R.save_model(g.mesh(), "39_multi_resolution.ply")


if __name__ == "__main__":
    main()
