"""Sculpt layers — a pass you can dial back after you have made it.

This is ZBrush's layers, on a voxel grid. Bracket a run of strokes and the grid
remembers what those strokes *changed*, so their strength stays adjustable long
after they are finished.

**It is not undo.** Undo is a stack you pop: the last thing you did, then the
one before, and once you have carried on working the early moves are out of
reach. A sculpt layer is *addressable* — make the wrinkles, keep sculpting for
an hour, then decide the wrinkles were too strong and take them to 40%. Nothing
made after them is disturbed.

**A layer stores what its pass DID, not the brushes that did it.** Dialling a
layer replays recorded cells; it does not re-run the strokes. So a pass whose
result depended on the layer beneath it keeps the result it recorded when that
layer is dialled away. That is what a layer stack means, and what ZBrush does —
re-running would make a layer's content depend on whatever happens to sit below
it, which is the opposite of addressable. This script demonstrates it rather
than asserting it.

**The interesting part is what a fraction MEANS on binary occupancy.** A voxel
is there or it is not; there is no "40% of a voxel". ZBrush layers interpolate
vertex offsets, which is a continuous quantity — this has none. So a fractional
strength is a reproducible fraction of the *cells*, chosen by the same
cell-coordinate hash the falloff brushes dither with. Three properties come out
of that choice, and all three are checked below:

- the same strength picks the same cells on every platform and every run, since
  the hash is over integer cell coordinates rather than an RNG draw;
- raising the strength **adds** cells to the ones already showing instead of
  reshuffling which ones are chosen, because each layer holds a fixed seed;
- 0 and 1 are **exact** — the grid without the pass, and the pass applied
  directly — because the dither admits none and all at the ends.
"""

import numpy as np

import pyclay as clay

import _render as R

VOXEL = 0.045
# Framed on the CROWN, which is where the pass lands. A camera that fits the
# whole form makes five tiles that differ by a few hundred voxels look identical.
EYE, TARGET = (1.35, 1.05, 1.65), (0.0, 0.30, 0.0)


def base_form():
    """A blocked-out head-ish mass, voxelised. The passes below go onto this."""
    doc = clay.Document()
    layer = doc.add_sdf_layer("form")
    layer.add(clay.Sphere(r=0.62), color="#c08a5a")
    layer.add(clay.Capsule(a=(0, -0.30, 0), b=(0, -0.95, 0), r=0.34),
              blend=clay.Smooth(0.18))
    layer.add(clay.Box(size=(0.30, 0.20, 0.36), position=(0, -0.10, 0.50)),
              blend=clay.Smooth(0.10))

    grid = clay.VoxelGrid(VOXEL)
    grid.rasterize(doc)
    return grid


def cells_of(grid):
    """The occupied set, as a comparable object."""
    lo, hi = grid.bounds()
    out = set()
    for z in range(lo[2], hi[2] + 1):
        for y in range(lo[1], hi[1] + 1):
            for x in range(lo[0], hi[0] + 1):
                if grid.get((x, y, z)):
                    out.add((x, y, z))
    return out


def detail_pass(grid):
    """A run of strokes worth being able to regret: a ridge over the crown.

    The dabs sit ON the surface rather than inside it. Inflate grows what is
    already there, so a footprint entirely in the interior has no surface to
    work from and changes nothing at all — a perfectly valid call that does
    nothing, which is easy to mistake for a broken layer."""
    r = 0.62 / VOXEL  # the crown sphere, in cells
    for angle in np.linspace(-1.0, 1.0, 9):
        cell = (int(round(np.sin(angle) * r)), int(round(np.cos(angle) * r)), 0)
        grid.sculpt_inflate(cell, size=9, amount=3, falloff="linear", strength=1.0)


def main():
    R.banner("52 sculpt layers — a pass you can dial back")

    grid = base_form()
    base = grid.occupied_count
    print(f"  blocked-out form: {base} voxels at {VOXEL} cell size")

    # --- record a pass -------------------------------------------------------
    # The context manager is the point: a raising stroke loop cannot leave the
    # grid recording, and every later edit silently joining an abandoned pass
    # is the bug that shape prevents.
    with grid.sculpt_layer("crown ridges") as ridges:
        detail_pass(grid)
    print(f"\n  recorded layer {ridges} '{grid.sculpt_layer_name(ridges)}': "
          f"{grid.sculpt_layer_cell_count(ridges)} cells changed, "
          f"{grid.occupied_count - base:+d} voxels")

    # --- the ends of the slider are EXACT ------------------------------------
    full = cells_of(grid)
    grid.set_sculpt_layer_strength(ridges, 0.0)
    off = cells_of(grid)
    grid.set_sculpt_layer_strength(ridges, 1.0)
    on = cells_of(grid)

    plain = base_form()
    if off != cells_of(plain):
        raise SystemExit("strength 0 must be the grid without the pass, exactly")
    if on != full:
        raise SystemExit("strength 1 must be the pass applied directly, exactly")
    print("  strength 0 is the form without the pass, exactly")
    print("  strength 1 is the pass applied directly, exactly")

    # --- and a fraction is a reproducible fraction of the CELLS --------------
    print("\n  what a fraction means on binary occupancy:")
    print(f"    {'strength':>10}{'voxels':>10}{'of the pass':>14}")
    shown = {}
    for s in (0.0, 0.25, 0.5, 0.75, 1.0):
        grid.set_sculpt_layer_strength(ridges, s)
        shown[s] = cells_of(grid)
        gained = len(shown[s]) - len(off)
        total = len(on) - len(off)
        print(f"    {s:>10.2f}{grid.occupied_count:>10}{gained / total * 100:>13.0f}%")

    # Monotone: dialling up ADDS to what was already showing. A layer with a
    # re-drawn seed would satisfy the counts above and fail this.
    for lo, hi in zip((0.0, 0.25, 0.5, 0.75), (0.25, 0.5, 0.75, 1.0)):
        if not shown[lo] <= shown[hi]:
            raise SystemExit(f"dialling {lo} -> {hi} reshuffled cells instead of adding")
    print("    every step is a SUPERSET of the one below — dialling up adds,")
    print("    it does not reshuffle. Each layer keeps one fixed seed.")

    # Reproducible: the same strength, rebuilt from scratch, picks the same
    # cells. This is the cross-platform claim, made locally.
    rebuilt = base_form()
    with rebuilt.sculpt_layer("crown ridges"):
        detail_pass(rebuilt)
    rebuilt.set_sculpt_layer_strength(0, 0.5)
    if cells_of(rebuilt) != shown[0.5]:
        raise SystemExit("the same strength must pick the same cells")
    print("    and a second grid built the same way picks the same cells at 0.50")

    # --- the pictures --------------------------------------------------------
    tiles = []
    for s in (0.0, 0.25, 0.5, 0.75, 1.0):
        grid.set_sculpt_layer_strength(ridges, s)
        tiles.append(R.render_voxels_array(grid, eye=EYE, target=TARGET,
                                           width=250, height=250))
    R.contact_sheet(tiles, "52_sculpt_layers.png", columns=5,
                    caption="one pass at strength 0.00, 0.25, 0.50, 0.75, 1.00")

    # --- a stack, and what removing from the middle means --------------------
    grid.set_sculpt_layer_strength(ridges, 1.0)
    with grid.sculpt_layer("chin") as chin:
        # On the jaw block's FRONT face, for the same reason.
        for dx in (-3, 0, 3):
            grid.sculpt_inflate((dx, -2, 16), size=9, amount=3, falloff="linear")
    print(f"\n  a second pass on top: layer {chin} "
          f"'{grid.sculpt_layer_name(chin)}', "
          f"{grid.sculpt_layer_cell_count(chin)} cells")

    both = cells_of(grid)
    grid.set_sculpt_layer_visible(ridges, False)
    without_ridges = cells_of(grid)
    if grid.sculpt_layer_visible(ridges):
        raise SystemExit("a hidden layer must report itself hidden")
    grid.set_sculpt_layer_visible(ridges, True)
    if not grid.sculpt_layer_visible(ridges):
        raise SystemExit("a shown layer must report itself shown")
    if cells_of(grid) != both:
        raise SystemExit("hiding and showing a layer must be reversible")
    print(f"  hiding the lower one: {len(both)} -> {len(without_ridges)} voxels, "
          f"and showing it restores exactly")

    # Merging down folds two into one that still dials.
    grid.merge_sculpt_layer_down(chin)
    if grid.sculpt_layer_count != 1 or cells_of(grid) != both:
        raise SystemExit("merging down must not change what is visible")
    grid.set_sculpt_layer_strength(0, 0.0)
    if cells_of(grid) != off:
        raise SystemExit("the merged layer must still dial to nothing")
    grid.set_sculpt_layer_strength(0, 1.0)
    print(f"  merged into one layer '{grid.sculpt_layer_name(0)}' "
          f"({grid.sculpt_layer_cell_count(0)} cells) that still dials to nothing")

    # --- and it survives the file -------------------------------------------
    doc = clay.Document()
    saved = doc.add_voxel_layer("form", voxel_size=VOXEL)
    for cell in sorted(cells_of(plain)):
        saved.set(cell, plain.get(cell))
    with saved.sculpt_layer("crown ridges"):
        detail_pass(saved)
    saved.set_sculpt_layer_strength(0, 0.6)

    path = R.output_path("52_sculpt_layers.clayspace")
    doc.save(str(path))
    back = clay.load(str(path)).voxel_layer("form")
    if back.sculpt_layer_count != 1 or abs(back.sculpt_layer_strength(0) - 0.6) > 1e-6:
        raise SystemExit("sculpt layers must survive the file")
    if back.occupied_count != saved.occupied_count:
        raise SystemExit("the reloaded grid must be the same voxels")
    back.set_sculpt_layer_strength(0, 1.0)
    saved.set_sculpt_layer_strength(0, 1.0)
    if back.occupied_count != saved.occupied_count:
        raise SystemExit("the reloaded layer must still dial")
    print(f"\n  saved and reloaded: layer '{back.sculpt_layer_name(0)}' at 0.60,")
    print("  still dialable — the file stores the DIFF, not the result")


if __name__ == "__main__":
    main()
