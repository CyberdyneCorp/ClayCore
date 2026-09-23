"""Voxel sculpt-layer operations as undo steps, through pyclay
(unify-the-undo-history 3.3, 3.4, 4.4, and the 2.4 decision).

The same claims the C suite makes, at the level a script writes: a dial undone
restores the DIAL — the slider and the cells — and not the pass under it; a
merge-down undone restores both layers. `Document.to_bytes()` covers the grid
and its layer stack, so "restored" is compared as bytes.
"""

import pytest

pyclay = pytest.importorskip("pyclay")


def _doc(undo=True):
    doc = pyclay.Document()
    grid = doc.add_voxel_layer("blocks", voxel_size=0.1)
    grid.fill_box((-6, -6, -6), (6, 6, 6), 1)
    if undo:
        doc.enable_undo()
    return doc, grid


def _pass(grid, name, y, amount):
    with grid.sculpt_layer(name):
        grid.sculpt_inflate((0, y, 0), size=9, amount=amount)


def test_undoing_a_dial_restores_the_dial_not_the_pass():
    doc, grid = _doc()
    _pass(grid, "wrinkles", 6, 2)
    full, cells = doc.to_bytes(), grid.sculpt_layer_cell_count(0)
    assert doc.undo_depth == 2      # the layer's creation, and the pass

    grid.set_sculpt_layer_strength(0, 0.4)
    dialled = doc.to_bytes()
    assert doc.undo_depth == 3

    assert doc.undo()
    assert grid.sculpt_layer_strength(0) == 1.0
    assert grid.sculpt_layer_count == 1
    assert grid.sculpt_layer_cell_count(0) == cells
    assert doc.to_bytes() == full

    assert doc.redo()
    assert grid.sculpt_layer_strength(0) == pytest.approx(0.4)
    assert doc.to_bytes() == dialled


def test_undoing_a_pass_takes_it_out_of_the_layer_and_then_the_layer():
    # Regression (#642): the undo reverted the pass's cells and left the
    # layer's record listing them, so the next dial put them back.
    doc, grid = _doc()
    start = doc.to_bytes()
    with grid.sculpt_layer("wrinkles"):
        opened, occupied = doc.to_bytes(), grid.occupied_count
        grid.sculpt_inflate((0, 6, 0), size=9, amount=2)

    assert doc.undo()
    assert grid.sculpt_layer_cell_count(0) == 0
    assert doc.to_bytes() == opened
    grid.set_sculpt_layer_strength(0, 0.5)
    assert grid.occupied_count == occupied
    dialled = doc.to_bytes()
    assert doc.undo()               # the dial
    assert doc.undo()               # the layer itself
    assert grid.sculpt_layer_count == 0
    assert doc.to_bytes() == start

    # The dial was a new edit, so the redo side holds the layer and the dial.
    assert doc.redo() and doc.redo()
    assert doc.to_bytes() == dialled
    assert not grid.recording_sculpt_layer  # a redone creation comes back closed


def test_undoing_a_merge_down_restores_both_layers():
    doc, grid = _doc()
    _pass(grid, "lower", 6, 2)
    _pass(grid, "upper", 8, 2)       # overlaps the lower pass AND reaches past it
    before = doc.to_bytes()
    counts = [grid.sculpt_layer_cell_count(i) for i in range(2)]

    grid.merge_sculpt_layer_down(1)
    merged = doc.to_bytes()
    assert grid.sculpt_layer_count == 1

    assert doc.undo()
    assert grid.sculpt_layer_count == 2
    assert [grid.sculpt_layer_name(i) for i in range(2)] == ["lower", "upper"]
    assert [grid.sculpt_layer_cell_count(i) for i in range(2)] == counts
    assert doc.to_bytes() == before
    assert doc.redo()
    assert doc.to_bytes() == merged


def test_visibility_reorder_and_removal_are_each_one_step():
    doc, grid = _doc()
    _pass(grid, "a", 6, 2)
    _pass(grid, "b", 6, -2)
    base = doc.undo_depth
    grid.set_sculpt_layer_visible(1, False)
    grid.move_sculpt_layer(1, 0)
    grid.remove_sculpt_layer(0)
    assert doc.undo_depth == base + 3
    for _ in range(3):
        assert doc.undo()
    assert [grid.sculpt_layer_name(i) for i in range(2)] == ["a", "b"]
    assert grid.sculpt_layer_visible(1)


def test_enabling_undo_mid_session_starts_an_empty_history():
    doc, grid = _doc(undo=False)
    _pass(grid, "before", 6, 2)
    doc.enable_undo()
    assert doc.undo_depth == 0
    assert not doc.undo()
    grid.set_sculpt_layer_strength(0, 0.5)
    doc.enable_undo()               # a second enable keeps what it has
    assert doc.undo_depth == 1


def test_a_gesture_end_after_a_mid_gesture_enable_folds_nothing():
    doc, grid = _doc(undo=False)
    with pytest.raises(RuntimeError):
        doc.begin_undo_group()      # refused while undo is off
    doc.enable_undo()
    grid.set((20, 0, 0), 1)
    grid.set((21, 0, 0), 1)
    doc.end_undo_group()            # closes a bracket that never opened
    assert doc.undo_depth == 2
