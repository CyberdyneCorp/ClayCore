"""The remaining voxel verbs, and repair.

Four sculpting verbs already existed — smooth, inflate, flatten, pinch. These
are the four the study catalogues that were missing, plus the pre-bake repair
pair.

Every verb below takes an optional `mask=`, and none of them is shown with one
here — that lives in `11_masks.py`, which runs all ten twice, masked and not.
The gating is not per-verb: it sits in the one footprint walk they share, so a
verb added after the mask was inherits it rather than having to remember it.

Two things here are worth reading rather than only looking at.

**fill-cavities is not a morphological closing.** Closing is the textbook
answer and it was the first attempt; the code found two reasons it is wrong. A
ball of radius r *fits into* a dent wider than r, so a larger structuring
element fills **less**, not more — a radius-2 closing declines to fill a 2x2
dent that a radius-1 closing seals. And a closing cannot seal a one-cell
perforation in a one-cell wall at all, because the erosion reaches through from
the void behind and reopens every hole the dilation just closed. Both are
exactly the cases this is for.

What works is local and blunt: an empty cell with at least four of its six face
neighbours occupied is inside a pocket. A flat face gives one, a concave edge
two, a corner three — so four is the line between "irregular surface" and
"hole". This example shows both sides of that line, because the line is the
design.

**Enclosure is decided, not guessed at.** A local neighbourhood cannot tell an
enclosed pocket from a deep dent, so fill-voids floods empty cells inward from
outside the bounds; whatever it cannot reach is enclosed. The cutaway renders
below are of the same grids, sliced open, so the interior is actually visible.
"""

import numpy as np

import pyclay as clay

import _render as R

VOXEL_SIZE = 0.1


def slab(thickness=4, half=8):
    g = clay.VoxelGrid(voxel_size=VOXEL_SIZE)
    g.fill_box((-half, 0, -half), (half, thickness - 1, half), g.palette_add("#9aa4b0"))
    return g


def hollow_box(half=5, colour="#c8703a"):
    g = clay.VoxelGrid(voxel_size=VOXEL_SIZE)
    g.fill_box((-half, -half, -half), (half, half, half), g.palette_add(colour))
    g.fill_box((-half + 1, -half + 1, -half + 1), (half - 1, half - 1, half - 1), 0)
    return g


def cutaway(src, half=5):
    """A copy with one quadrant removed, so the interior can be seen at all."""
    out = clay.VoxelGrid(voxel_size=VOXEL_SIZE)
    colour = out.palette_add("#c8703a")
    for x in range(-half, half + 1):
        for y in range(-half, half + 1):
            for z in range(-half, half + 1):
                if x > 0 and z > 0:
                    continue                      # the quadrant we slice away
                if src.get((x, y, z)) != 0:
                    out.set((x, y, z), colour)
    return out


def tile(grid, **kwargs):
    eye, target = R.voxel_camera(grid, VOXEL_SIZE, **kwargs)
    return R.render_voxels_array(grid, eye=eye, target=target, width=210, height=195)


def main():
    R.banner("15 voxel verbs and repair")

    # --- fill cavities, and the line it draws --------------------------------
    pocket = slab()
    pocket.fill_box((0, 2, 0), (0, 3, 0), 0)      # one across, two deep
    dent = slab()
    dent.fill_box((-1, 3, -1), (0, 3, 0), 0)      # two across, one deep

    tiles = [tile(pocket, azimuth=32.0, elevation=34.0)]
    filled = slab()
    filled.fill_box((0, 2, 0), (0, 3, 0), 0)
    filled.sculpt_fill_cavities((0, 2, 0), 9, passes=2, shape="cube")
    tiles.append(tile(filled, azimuth=32.0, elevation=34.0))
    tiles.append(tile(dent, azimuth=32.0, elevation=34.0))
    dent_after = slab()
    dent_after.fill_box((-1, 3, -1), (0, 3, 0), 0)
    dent_after.sculpt_fill_cavities((0, 3, 0), 9, passes=2, shape="cube")
    tiles.append(tile(dent_after, azimuth=32.0, elevation=34.0))

    print(f"  a 1-cell pocket 2 deep  -> filled: {filled.get((0, 2, 0)) != 0}")
    print(f"  a 2-cell dent 1 deep    -> filled: {dent_after.get((0, 3, 0)) != 0}"
          f"  (three neighbours, below the threshold — by design)")
    if filled.get((0, 2, 0)) == 0 or dent_after.get((0, 3, 0)) != 0:
        raise SystemExit("fill-cavities no longer draws the line it documents")
    R.contact_sheet(tiles, "15_fill_cavities.png", columns=4,
                    caption="pocket before/after, then a shallow dent before/after "
                            "(left alone on purpose)")

    # --- scrape vs flatten ---------------------------------------------------
    def bumpy():
        g = slab()
        accent = g.palette_add("#d08a52")
        for x in range(-5, 6, 2):
            g.set((x, 4, 0), accent)
        g.set((0, 5, 0), accent)
        return g

    tiles = [tile(bumpy(), azimuth=24.0, elevation=22.0)]
    flattened = bumpy()
    flattened.sculpt_flatten((0, 4, 0), 13, normal=(0, 1, 0), shape="cube")
    tiles.append(tile(flattened, azimuth=24.0, elevation=22.0))
    scraped = bumpy()
    scraped.sculpt_scrape((0, 4, 0), 13, normal=(0, 1, 0), shape="cube")
    tiles.append(tile(scraped, azimuth=24.0, elevation=22.0))
    print(f"  bumpy {bumpy().occupied_count} -> flatten {flattened.occupied_count} "
          f"-> scrape {scraped.occupied_count} cells")
    R.contact_sheet(tiles, "15_scrape.png", columns=3,
                    caption="bumpy, flattened, and scraped (flatten + smooth in one pass)")

    # --- smudge vs grab ------------------------------------------------------
    def block():
        g = clay.VoxelGrid(voxel_size=VOXEL_SIZE)
        g.fill_box((-6, -6, -6), (6, 6, 6), g.palette_add("#7f8a94"))
        return g

    tiles = [tile(block(), azimuth=30.0, elevation=26.0)]
    smudged = block()
    smudged.sculpt_smudge((6, 0, 0), 9, displacement=(0.4, 0, 0), shape="cube")
    tiles.append(tile(smudged, azimuth=30.0, elevation=26.0))
    grabbed = block()
    grabbed.sculpt_grab((6, 0, 0), 9, displacement=(0.4, 0, 0), shape="cube")
    tiles.append(tile(grabbed, azimuth=30.0, elevation=26.0))

    interior_intact = all(smudged.get((x, 0, 0)) != 0 for x in range(-6, 4))
    print(f"  smudge leaves the interior alone: {interior_intact}   "
          f"(grab moves the lump, smudge smears the skin)")
    if not interior_intact:
        raise SystemExit("smudge moved more than the surface")
    R.contact_sheet(tiles, "15_smudge.png", columns=3,
                    caption="untouched, smudged, grabbed — the same displacement")

    # --- carve with an alpha -------------------------------------------------
    # A recognisable stamp, so the carve is obviously the alpha's shape.
    n = 16
    yy, xx = np.mgrid[0:n, 0:n]
    ring = (((xx - n / 2) ** 2 + (yy - n / 2) ** 2) ** 0.5)
    alpha = ((ring > n * 0.22) & (ring < n * 0.42)).astype(np.float32)

    carved = block()
    carved.sculpt_carve_alpha((0, 6, 0), 13, alpha=alpha, direction=(0, 1, 0), shape="cube")
    print(f"  the ring alpha removed {block().occupied_count - carved.occupied_count} cells")
    R.contact_sheet([tile(block(), azimuth=18.0, elevation=52.0),
                     tile(carved, azimuth=18.0, elevation=52.0)],
                    "15_carve_alpha.png", columns=2,
                    caption="a ring-shaped alpha carved into the top face")

    # --- repair: report, close, fill -----------------------------------------
    shell = hollow_box()
    shell.set((5, 0, 0), 0)                       # pierce one wall
    pierced = shell.repair_report()
    print(f"  pierced shell: {pierced['enclosed_voids']} enclosed voids, "
          f"airtight={pierced['airtight']}  (the outside reaches in)")

    sealed = hollow_box()
    sealed.set((5, 0, 0), 0)
    sealed.repair_close_holes(passes=1)
    after_close = sealed.repair_report()
    print(f"  after close-holes: {after_close['enclosed_voids']} enclosed void of "
          f"{after_close['largest_void']} cells, airtight={after_close['airtight']}")

    solid = hollow_box()
    solid.set((5, 0, 0), 0)
    solid.repair_close_holes(passes=1)
    solid.repair_fill_voids()
    after_fill = solid.repair_report()
    print(f"  after fill-voids : airtight={after_fill['airtight']}, "
          f"{solid.occupied_count} cells")

    if pierced["airtight"] is not True or after_close["enclosed_voids"] != 1 \
            or after_fill["airtight"] is not True:
        raise SystemExit("repair no longer reports what it claims")

    # Sliced open, because the whole point is interior geometry.
    R.contact_sheet([tile(cutaway(shell), azimuth=38.0, elevation=28.0),
                     tile(cutaway(sealed), azimuth=38.0, elevation=28.0),
                     tile(cutaway(solid), azimuth=38.0, elevation=28.0)],
                    "15_repair.png", columns=3,
                    caption="cut away: pierced shell, holes closed, voids filled")

    # An open cavity is not a void: the outside reaches it, so it survives.
    open_box = hollow_box()
    open_box.fill_box((5, -3, -3), (5, 3, 3), 0)   # a wide mouth
    before = open_box.occupied_count
    open_box.repair_fill_voids()
    print(f"  a box with a wide mouth is left alone by fill-voids: "
          f"{open_box.occupied_count == before}")
    if open_box.occupied_count != before:
        raise SystemExit("fill-voids filled a cavity the outside can reach")

    R.save_model(solid.mesh(), "15_repaired.ply")


if __name__ == "__main__":
    main()
