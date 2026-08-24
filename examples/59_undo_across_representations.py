"""One Ctrl+Z, whichever half of the engine made the edit.

This library has three history mechanisms — a command stack for the SDF edit
list, sculpt layers for voxel grids, sparse vertex deltas for mesh layers —
and until `unify-the-undo-history` no single undo step spanned two of them.
`Document.undo()` reached the first and silently did nothing for the other two.

That is a sculpting-app problem rather than a kernel one, and it is not
hypothetical here: `42_representation_round_trip` blocks out in SDF, rasterizes
to voxels, smooths the seam with a voxel verb and converts back. Every one of
those transitions is a place a user reaches for undo, and two of the three used
to reverse the wrong thing or nothing at all.

This script does the same crossing and then walks it backwards, asserting after
every step that the thing that came back is the thing that was last done.

It also pins the two properties that make the feature usable rather than merely
present:

  - a sculpt verb is ONE step, not one per cell it touched, and
  - an edit that changed nothing is not a step at all, so a menu built from
    `undo_depth` never offers an undo that does nothing.

Masks used to be the boundary here — a fourth representation with no history
mechanism, which the audit that counted three did not count. They are steps
now, so this script paints one and undoes it like anything else. What remains
outside is narrower: operations that destroy history itself, and anything a
host does that the engine never sees.
"""

import numpy as np

import pyclay as clay

import _render as R

CELL = 0.05
EYE, TARGET = (2.4, 1.6, 2.4), (0.0, 0.0, 0.0)


def probe(doc, points):
    return doc.eval(np.array(points, dtype=np.float32))


def main():
    R.banner("59 one undo, across three representations")

    doc = clay.Document()
    sdf = doc.add_sdf_layer("blockout")
    blocks = doc.add_voxel_layer("blocks", voxel_size=CELL)
    doc.enable_undo()

    origin = [[0.0, 0.0, 0.0]]

    # --- 1. an SDF edit ---------------------------------------------------
    sdf.add(clay.Sphere(r=0.5), color="#c8b28c")
    inside_sphere = probe(doc, origin)[0]
    if inside_sphere >= 0:
        raise SystemExit("the sphere should enclose the origin")
    print(f"  1. sdf item        -> field at origin {inside_sphere:+.3f}, "
          f"undo_depth {doc.undo_depth}")

    # --- 2. a voxel edit, which used to be invisible to undo ---------------
    blocks.fill_box((0, 0, 0), (6, 6, 6), 1)
    filled = blocks.occupied_count
    print(f"  2. voxel fill      -> {filled} cells, undo_depth {doc.undo_depth}")

    # --- 3. a voxel VERB, which must be one step and not one per cell ------
    blocks.sculpt_smooth((0, 0, 0), size=5)
    smoothed = blocks.occupied_count
    if smoothed == filled:
        raise SystemExit("the smooth should have changed the corner")
    print(f"  3. voxel smooth    -> {smoothed} cells "
          f"({filled - smoothed} removed), undo_depth {doc.undo_depth}")
    if doc.undo_depth != 3:
        raise SystemExit(
            f"three edits should be three steps, not {doc.undo_depth} — a verb that "
            "recorded one step per cell would make undo unusable")

    R.render(doc, "59_before_undo.png", eye=EYE, target=TARGET,
             caption="an SDF item, a voxel fill and a voxel verb")

    # --- an edit that changes nothing is not a step ------------------------
    before = doc.undo_depth
    blocks.erase((500, 500, 500))          # nowhere near anything
    if doc.undo_depth != before:
        raise SystemExit(
            "a write that changed no cell must not become a step: a menu built from "
            "undo_depth would then offer an undo that does nothing")
    print(f"  .  no-op erase     -> undo_depth still {doc.undo_depth}")

    # --- walk it backwards, newest first ----------------------------------
    if not doc.undo():
        raise SystemExit("undo should have reversed the smooth")
    if blocks.occupied_count != filled:
        raise SystemExit("the whole verb should come back in one step")
    print(f"  <- undo 1          -> {blocks.occupied_count} cells (the smooth, whole)")

    if not doc.undo():
        raise SystemExit("undo should have reversed the fill")
    if blocks.occupied_count != 0:
        raise SystemExit("the fill should be gone")
    if probe(doc, origin)[0] != inside_sphere:
        raise SystemExit("undoing voxel work must not disturb the SDF layer")
    print(f"  <- undo 2          -> {blocks.occupied_count} cells, "
          f"sdf untouched at {probe(doc, origin)[0]:+.3f}")

    if not doc.undo():
        raise SystemExit("undo should have reversed the sdf item")
    if probe(doc, origin)[0] <= 0:
        raise SystemExit("the sphere should be gone")
    # An EMPTY layer evaluates to the far-field sentinel rather than to a
    # distance, because there is no surface to be a distance from. Printed as
    # what it means rather than as 3.4e38, which reads like a bug.
    empty = probe(doc, origin)[0]
    print(f"  <- undo 3          -> the layer is empty again "
          f"(field reports {'no surface' if empty > 1e30 else f'{empty:+.3f}'}), "
          f"undo_depth {doc.undo_depth}")

    if doc.undo():
        raise SystemExit("there should be nothing left to undo")

    # --- and forwards again ------------------------------------------------
    for _ in range(3):
        if not doc.redo():
            raise SystemExit("redo should restore every step in order")
    if blocks.occupied_count != smoothed:
        raise SystemExit("redo must land exactly where undo started")
    if probe(doc, origin)[0] != inside_sphere:
        raise SystemExit("redo must restore the sdf layer too")
    print(f"  -> redo x3         -> {blocks.occupied_count} cells, "
          f"field {probe(doc, origin)[0]:+.3f} — back where we started")

    R.render(doc, "59_after_redo.png", eye=EYE, target=TARGET,
             caption="three undos and three redos later: byte for byte the same")

    # --- the fourth representation, which used to be the boundary ----------
    # A mask WAS a fourth representation with no history mechanism: twenty
    # mutating entry points and not one command variant, so painting one ended
    # the undoable run. It records now, and this is the assertion that used to
    # say the opposite — it was written to fail the day this changed.
    freeze = doc.add_mask("blockout", cell_size=CELL)
    depth_before_mask = doc.undo_depth
    freeze.fill(((-0.2, -0.2, -0.2), (0.2, 0.2, 0.2)), 1.0)
    painted = freeze.painted_count
    if doc.undo_depth != depth_before_mask + 1:
        raise SystemExit("a mask edit should be one undo step")
    print(f"  4. mask painted    -> {painted} cells, undo_depth {doc.undo_depth}")

    if not doc.undo():
        raise SystemExit("undo should have reversed the mask edit")
    if freeze.painted_count != 0:
        raise SystemExit("the painted cells should be gone")
    print(f"  <- undo 4          -> {freeze.painted_count} mask cells — a mask undoes "
          "like anything else now")
    doc.redo()

    print("\n  one Ctrl+Z, whichever half of the engine made the edit.")


if __name__ == "__main__":
    main()
