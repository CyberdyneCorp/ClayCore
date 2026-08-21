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

import pathlib
import re

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


# Bookkeeping that shares the sculpt_ prefix without being a verb: these do not
# reshape anything, they record and dial a PASS made of the verbs below.
# 52_sculpt_layers is their page, and naming it here keeps the exemption on the
# record instead of widening the scan until it stops catching anything.
SHOWN_ELSEWHERE = {
    "sculpt_layer": "52_sculpt_layers",
    "sculpt_layer_cell_count": "52_sculpt_layers",
    "sculpt_layer_count": "52_sculpt_layers",
    "sculpt_layer_name": "52_sculpt_layers",
    "sculpt_layer_strength": "52_sculpt_layers",
    "sculpt_layer_visible": "52_sculpt_layers",
    "sculpt_layer_bytes": "52_sculpt_layers",
    "sculpt_layers_bytes": "52_sculpt_layers",
    "begin_sculpt_layer": "52_sculpt_layers",
    "end_sculpt_layer": "52_sculpt_layers",
    "recording_sculpt_layer": "52_sculpt_layers",
    "move_sculpt_layer": "52_sculpt_layers",
    "remove_sculpt_layer": "52_sculpt_layers",
    "merge_sculpt_layer_down": "52_sculpt_layers",
    "set_sculpt_layer_strength": "52_sculpt_layers",
    "set_sculpt_layer_visible": "52_sculpt_layers",
}


def sculpt_verbs():
    """Every verb the binding exposes, read from it rather than listed here, so
    a new one shows up as a gap instead of being forgotten.

    Matched on `"sculpt" in m` rather than a `sculpt_` PREFIX, because a prefix
    is what the sculpt-layer API is named around instead of with:
    `move_sculpt_layer`, `remove_sculpt_layer` and `recording_sculpt_layer` all
    carry the word in the middle, and all three sat with no example at all while
    this scan reported full coverage. A gate that reads a name has to match the
    way names are actually built, or it certifies the subset it happens to spell.
    """
    return {m for m in dir(clay.VoxelGrid)
            if (m.startswith("repair_") or "sculpt" in m)
            and not m.startswith("_")
            and m not in SHOWN_ELSEWHERE}


def check_coverage():
    """Every sculpt and repair verb is exercised on this page.

    The pictures prove the verbs RAN. They cannot prove one is still reachable
    from the bindings at all — a verb that lost its example leaves no gap in a
    contact sheet. Read from the binding rather than from a list here, so a
    verb added later shows up as a gap instead of being forgotten.
    """
    src = pathlib.Path(__file__).read_text()
    missing = {v for v in sculpt_verbs() if f".{v}(" not in src}
    if missing:
        raise SystemExit(f"voxel verbs with no example here: {sorted(missing)}")
    # ...and an entry that points at a page which no longer covers it is the
    # same gap wearing a different hat.
    for verb, page in sorted(SHOWN_ELSEWHERE.items()):
        other = pathlib.Path(__file__).with_name(f"{page}.py")
        # Attribute access, not a call: some of these are properties, and
        # requiring parentheses would report a page that does show them.
        used = other.is_file() and re.search(rf"\.{verb}(?![A-Za-z0-9_])", other.read_text())
        if not used:
            raise SystemExit(f"{verb} is recorded as shown by {page}, which does not show it")
    print(f"  covered all {len(sculpt_verbs())} sculpt and repair verbs")


def check_every_verb_bites():
    """Each verb, given a target it should act on, changes cells.

    `change_count` rather than `occupied_count`: grab and magnify move material
    without adding any, so an occupancy tally is identical across an edit that
    moved a whole lump. A verb that silently became a no-op is otherwise
    invisible — every one of them is ALLOWED to do nothing, so nothing raises.
    """
    def block_grid():
        """A block with an uneven top: smooth and flatten need something to
        even OUT, and a perfectly flat face is already their fixed point."""
        g = clay.VoxelGrid(voxel_size=0.1)
        c = g.palette_add((0.8, 0.6, 0.4))
        g.fill_box((0, 0, 0), (9, 8, 9), c)
        for x in range(0, 10, 2):
            for z in range(0, 10, 3):
                g.fill_box((x, 9, z), (x, 10, z), c)   # spurs on the top face
        return g

    # ON the top face, not inside the block. Every verb here acts on SURFACE
    # cells, so a footprint buried in solid material legitimately changes
    # nothing — the first draft of this check put the brush at the centre and
    # "caught" six verbs that were working exactly as documented.
    at, size = (4, 9, 4), 7
    cases = {
        "sculpt_smooth":   lambda g: g.sculpt_smooth(at, size),
        "sculpt_inflate":  lambda g: g.sculpt_inflate(at, size, amount=1),
        "sculpt_flatten":  lambda g: g.sculpt_flatten(at, size, normal=(0, 1, 0)),
        "sculpt_pinch":    lambda g: g.sculpt_pinch(at, size),
        "sculpt_magnify":  lambda g: g.sculpt_magnify(at, size),
        "sculpt_scrape":   lambda g: g.sculpt_scrape(at, size, normal=(0, 1, 0)),
        "sculpt_smudge":   lambda g: g.sculpt_smudge(at, size, displacement=(0.5, 0, 0)),
        "sculpt_grab":     lambda g: g.sculpt_grab(at, size, displacement=(0.5, 0, 0)),
        "sculpt_carve_alpha": lambda g: g.sculpt_carve_alpha(
            at, size, alpha=np.ones((8, 8), dtype=np.float32), direction=(0, 1, 0)),
    }
    dead = []
    for name, run in cases.items():
        g = block_grid()
        before = g.change_count
        run(g)
        if g.change_count == before:
            dead.append(name)
    if dead:
        raise SystemExit(f"these verbs changed no cell on a solid block: {dead}")
    print(f"  all {len(cases)} sculpt verbs changed cells on a block they should bite")

    # fill_cavities and the repair verbs need a defect to act on rather than a
    # solid block, which the pages above already build and measure.



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

    # --- the gesture that actually produces cavities -------------------------
    # Issue #18: a host shipped this verb as a brush and it "correctly did
    # nothing for every gesture a user is likely to make". The pits above are
    # the shape every test builds, and they are also why the verb looks
    # unreachable: nobody draws a 1-cell pit on purpose.
    #
    # The gesture that does produce them is the ordinary one. Occupancy is
    # binary, so any strength or falloff below 1 is DITHERED against a hash of
    # the cell coordinate — a soft stamp lays down a pepper of single-cell
    # holes THROUGH the material it deposits, and each one is a cavity by the
    # four-neighbour rule. A dragged soft stroke is therefore a cavity factory,
    # and the artist never sees the holes because greedy meshing renders six
    # faces around every one of them.
    soft = clay.VoxelGrid(voxel_size=VOXEL_SIZE)
    clay_c = soft.palette_add("#9aa4b0")
    for i in range(12):
        soft.set_brush((-6 + i, 0, 0), 13, clay_c, shape="sphere",
                       falloff="smooth", strength=0.65, seed=3)
    before_cells = soft.occupied_count
    before_tris = soft.mesh().triangle_count
    voids = soft.repair_report()["enclosed_voids"]
    changed = soft.change_count
    for x in range(-8, 9, 2):
        soft.sculpt_fill_cavities((x, 0, 0), 15, passes=2)
    added = soft.occupied_count - before_cells
    after_tris = soft.mesh().triangle_count

    print(f"  a dragged SOFT stroke: {before_cells} cells, "
          f"{voids} enclosed voids")
    print(f"    fill-cavities added {added} cells "
          f"(change_count agrees: {soft.change_count - changed})")
    print(f"    greedy mesh {before_tris} -> {after_tris} triangles "
          f"({100 * (after_tris - before_tris) / before_tris:+.0f}%)")

    # The two verbs are not substitutes, and this is the sharpest case: the
    # dither's holes are open to the outside, so the flood reaches every one of
    # them and fill-voids has nothing to do. Narrow is not the same as sealed.
    if voids != 0:
        raise SystemExit("a dithered stamp should leave OPEN holes, not sealed voids")
    if added <= 0 or after_tris >= before_tris:
        raise SystemExit("fill-cavities stopped paying for itself on a soft stroke")
    eye, target = R.voxel_camera(soft, VOXEL_SIZE, azimuth=28.0, elevation=30.0)
    R.render_voxels(soft, "15_soft_stroke_cavities.png", eye=eye, target=target,
                    caption="a dragged soft stroke, after fill-cavities")

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

    # --- what the pictures cannot check ---------------------------------------
    check_every_verb_bites()
    check_coverage()


if __name__ == "__main__":
    main()
