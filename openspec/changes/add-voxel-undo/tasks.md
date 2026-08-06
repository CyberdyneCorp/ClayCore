# Tasks — add-voxel-undo

- [x] 1.1 Interleaved journal on `clay_document` (UndoStep = scene flag + per-layer voxel diffs; invariant: scene-flagged steps == UndoStack depth)
- [x] 1.2 `apply_edit` mirrors scene steps 1:1 (group absorption, AppendStroke coalescing leaves the journal top in place)
- [x] 1.3 `scene::UndoStack::clear_redo()` so voxel edits invalidate scene redo like `perform()` does
- [x] 1.4 Region capture on every mutating voxel entry point (single cells, batches, fills, mirrored stamps, 3 brushes, 8 sculpt verbs — writes verified footprint-bounded in sculpt.cpp; grid-wide repair passes stay direct, out of scope)
- [x] 1.5 Journal-aware group brackets shared by the public entry points and `clay_layer_apply_stroke`'s internal group (found by the pre-existing stroke test)
- [x] 1.6 `clay_document_undo/redo/undo_state` pop and count journal steps; ABI 0.19 → 0.20 with header docs
- [x] 1.7 Tests: brush undo/redo round trip, scene/voxel interleaving, grouped drags with first-touch inverses, standalone-grid exemption, depth accounting + cross-kind redo invalidation, fills/mirrored/single-cell diffs, no-op brushes record nothing, sculpt verbs
- [ ] 1.8 Python bindings: pyclay's undo path does not route through the C journal — voxel undo is C-ABI only until pyclay adopts it (follow-up)
