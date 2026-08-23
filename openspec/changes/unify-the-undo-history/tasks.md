# Tasks: unify-the-undo-history

## 1. Measure before designing

- [x] 1.1 Confirm the three mechanisms and that no step spans two:
      `UndoStack` over `Command` for the edit list and layer state, sculpt
      layers for voxel grids, `mesh::VertexDeltas` for mesh layers
- [x] 1.2 Confirm the CAVEAT that corrects 1.2's first draft: `VoxelGrid::set`
      is the one choke point every verb funnels through, but its recording hook
      is guarded by `recording_`, true only between `begin_sculpt_layer` and
      `end_sculpt_layer`. An ordinary voxel edit therefore leaves NO record, so
      the replay machinery exists and the recording does not happen. Sculpt
      layers are the wrong lifetime for undo — they are artist-facing, named and
      reorderable — so this needs a second recording channel at the same choke
      point
- [x] 1.2b Confirm the inverse machinery itself exists — `SculptLayerRecord::changes`
      holds `{cell, before, after}` in pass order and `VoxelGrid::revert_from` /
      `apply_from` already replay it (privately); `VertexDeltas::revert` is
      public and already refuses a mesh of the wrong vertex count
- [x] 1.3 Confirm one object owns all three: `io::ClaySpaceDoc` holds the scene
      document, the voxel grids, the masks and the mesh layers
- [x] 1.4 Confirm `scene::Command` is a 19-alternative variant stored by value,
      so a voxel pass cannot join it without cost to every command
- [x] 1.5 Confirm the layering constraint, which decides the shape:
      `check_layering.py` allows `scene` only `{parallel, kernel, math, field}`,
      so a history that reverses a voxel pass AND a vertex delta cannot live
      beside `UndoStack`. `brush` is the only module that sees all three today
      and is the stroke engine, not a history. This change adds a `session`
      module, the way `parallel` was added when the pool could not be reached
- [x] 1.6 Confirm the history is currently owned per BINDING — `clay_document`
      holds a `unique_ptr<UndoStack>` and `PyDocument` holds its own — so two
      implementations already exist to drift, and a wider history is a wider
      surface to drift in
- [ ] 1.7 MEASURE what a mixed session holds, per representation per step. This
      is the input `add-history-budget` needs and that row assumes one mechanism

## 2. Decide

- [x] 2.1 DECIDED by the layering rule, not by taste: the session history
      WRAPS `UndoStack`. `UndoStack` needs only `scene` and stays there with its
      coalescing and grouping; the session history sits above and dispatches
- [x] 2.2 DECIDED: a distinct step KIND (Barrier) plus a horizon query.
      `undo_depth` counts only as far back as the nearest barrier, so a menu
      built from it never offers an undo that does nothing, and `next_barrier`
      names what is in the way. A flag on the depth would have made the depth
      mean two things at once
- [x] 2.3 DECIDED: by value. A step that borrows is a step whose validity
      depends on something it does not own, and the history outlives the
      sculptor that produced the deltas. The doubling is real and is what 1.7
      must measure
- [ ] 2.4 DECIDE and record: what enabling the history mid-session does, without
      changing what `enable_undo` means for the SDF path

## 3. Build

- [x] 3.0 A `session` module, and its line in `tools/check_layering.py`
- [x] 3.1 A second recording channel on `VoxelGrid::set`, independent of the
      sculpt-layer stack and written only when the history is enabled — plus a
      step-scoped revert/reapply beside the private `revert_from` / `apply_from`
      that already do the replay for sculpt layers
- [x] 3.2 The session history: an ordered log of steps, each naming its owner
      and carrying the token that reverses it
- [ ] 3.3 (DEFERRED — see 5.8) Two voxel step kinds — the pass, and a change to a pass (strength,
      visibility, order, merge-down) — so undoing a strength tweak does not
      remove the pass
- [ ] 3.4 (DEFERRED — see 5.8) Merge-down holds the folded record, since undoing it means restoring
      one. The only voxel step whose memory scales with the pass
- [x] 3.5 Redo discarded on the next edit, across representations
- [ ] 3.7 (CORRECTED — not needed) Move history ownership onto `io::ClaySpaceDoc`, so the two bindings
      share one implementation instead of instantiating one each
- [x] 3.6 Mesh steps refused rather than failed when a layer's vertex count has
      changed since the step was recorded

## 4. Prove it

- [x] 4.1 The scenarios in both spec deltas
- [x] 4.2 The regression this whole change is for: SDF stamp, voxel smooth, mesh
      grab, then three undos and three redos, asserting the document, the grid
      and the mesh each return to their starting and ending states
- [x] 4.3 Coalescing and grouping unchanged: a stroke of many stamps is still
      one step, over the golden corpus
- [ ] 4.4 (DEFERRED — see 5.8) A voxel strength change undoes without removing its pass
- [x] 4.5 A host that only ever edits SDF sees behaviour bit-identical to today

## 5. Reach it and say it

- [x] 5.1 C ABI — the existing undo entry points, now spanning three
      representations, plus whatever 2.2 decides
- [ ] 5.2 pyclay, so `check_binding_parity` stays clean
- [ ] 5.3 Swift smoke
- [x] 5.4 ABI minor bump and `docs/RELEASE.md`, stating plainly that undo now
      reverses more than it did — a behaviour change and a fix
- [ ] 5.5 `docs/05-claycore-library.md`: the history section, which does not
      exist, and which `correct-the-undo-scope` is the reason to write
- [ ] 5.6 A numbered example that crosses representations and undoes back
- [ ] 5.7 `openspec/ROADMAP.md`, and `correct-the-undo-scope` updated — it names
      this gap and will no longer be describing the tree

## 6. Corrections made while building

- [x] 6.1 CORRECTED: the barrier examples were wrong. **Consolidate IS
      undoable** — it takes an `UndoStack` and records through the command
      vocabulary — and rasterizing into a grid IS recorded once a sink is
      installed, since it writes through `set`. What genuinely is not recorded
      is **every mask edit** (`voxel::MaskField` is a FOURTH representation with
      twenty mutating ABI entry points and not one command variant, which
      `correct-the-undo-scope`'s "three mechanisms" framing did not count) and
      the operations that destroy history itself. Pinned by a test that
      consolidate becomes a step
- [x] 6.2 CORRECTED: the sink first journaled EVERY write, including ones that
      changed nothing, and a unit test enshrined that. The C-level test caught
      it — erasing an already-empty cell produced an undo step that undid
      nothing, the exact defect this channel exists to avoid. It now journals
      only writes `write_cell` reports as changing something
- [x] 6.3 CORRECTED: `UndoStack::begin_group` pushes its entry IMMEDIATELY, so
      the stack's depth grows at BEGIN and the commands inside append without
      growing it further. Detecting steps by "did the depth grow across this
      call" is therefore false for every part of a group, and a grouped edit
      recorded no step at all — four existing tests went red. `end_group` now
      reconciles against the stack. Regression test added
- [x] 6.4 CORRECTED: task 3.7 said history ownership must move to
      `io::ClaySpaceDoc` because "two bindings each owning a history is two
      implementations that will drift". Overstated: both instantiate the SAME
      engine class, so the implementation is already shared. The real drift risk
      is in the CALL SITES — one binding bracketing a voxel edit and the other
      forgetting — which moving ownership does not fix. An RAII bracket in each
      binding does, and that is what was built

## 7. Still open after this slice

- [ ] 5.8 Sculpt-layer PROPERTY changes (strength, visibility, order) are not
      steps. Their cell effect is restorable by replay, but the property value
      is not, so an undo would restore the pixels and not the setting — a
      partial undo, which is worse than none. Needs the second voxel step kind
      (3.3) and the merge-down record retention (3.4)
- [ ] 5.9 pyclay is NOT yet wired: its undo still spans the edit list alone.
      No regression for a Python host — that is today's behaviour — but the
      bindings now disagree about what undo means, which is the drift 6.4 names.
      Next change, immediately
