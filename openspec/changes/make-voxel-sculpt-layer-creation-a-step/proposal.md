# Proposal: a voxel sculpt layer's creation, and every edit inside it, is a step

## Why

`unify-the-undo-history` made the five operations ON a voxel sculpt layer undo
steps, and left the layer itself outside the history (#642).
`clay_voxel_begin_sculpt_layer` / `VoxelGrid::begin_sculpt_layer` adds a record
to the grid's stack that the session never sees, and the edits made inside the
layer record their CELLS as ordinary Voxel steps and not what they did to that
record. Two defects follow, both reproduced on `main` before this change through
the C ABI (`tests/unit/test_c_voxel_layer_history.cpp`):

| probe | on `main` | with this change |
|---|---|---|
| begin a layer, one inflate, undo: the layer's cell count | 110 | 0 |
| same, then dial the now-"empty" layer to 0.5: occupied cells vs before the inflate | 2251 vs 2197 — 54 undone cells put back | 2197 vs 2197 |
| same, document bytes after the undo vs before the inflate | differ | identical |
| snapshot, two passes and a dial, journal replayed onto the snapshot | refused after 2 of 3 events | all 5 applied, bytes identical |

The mesh stack never had this gap: `MultiresSurface::add_sculpt_layer` fills a
`SculptLayerProperty` (Structural), so creation is a step there.

## What changes

- `SculptLayerOp` gains two APPENDED kinds. **Begin** is a layer's creation:
  undo removes the top layer, redo puts it back. **Pass** is one edit made while
  a layer was recording: the cells it wrote, exactly as a Voxel step carries
  them, plus what it did to the layer's record — the length before, the entries
  it rewrote with their old and new `after`, and the entries it appended.
- `begin_sculpt_layer` takes the same optional record out-parameter as its five
  siblings, and both bindings pass one.
- `History::begin_voxel_step` also opens a *pass capture* on the grid; a step
  that closes with the record changed is recorded as a `VoxelLayerProperty`
  step (Pass) instead of a Voxel step. The journal carries it through the
  existing `VoxelLayerProperty` event, so replay needs no new path.
- No entry point is added and no signature changes at the C ABI; the version
  line does not move. The `SculptLayerOp` journal encoding gains one field; it
  has not shipped in a release (it arrived after 0.120.0), so no journal on disk
  carries the old layout.

## Host-visible consequences

- A pass inside a new layer is two undos: the edit, then the layer.
- Undoing a creation ends the recording; redoing it brings the layer back
  closed.
- With undo on, an edit inside a layer that changed no cell leaves the layer's
  record as it found it (see design.md for why).
