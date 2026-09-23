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
- [x] 1.7 MEASURED on Linux (x86_64, ratios only — the absolutes do not
      transfer to the device, per `docs/RELEASE.md`). **16 bytes per changed
      cell**; a size-9 sphere smooth on a corner journals 13 changes = 208 B.
      Write-path cost with the sink installed: **1.005x on `sculpt_smooth`**
      (the verb's own work dominates) and **1.77x on a bare loop of 64 raw
      `set` calls** — where the baseline is 21 ns per write, so the journal
      append is ~16 ns and the absolute stays at 0.001 ms. The first draft
      measured 1.26x on the verb because it probed `get(c)` for the previous
      value; `write_cell` already had it. See 7.1
- [x] 1.8 RE-SCOPED to #644, not measured. The same measurement ON THE DEVICE
      is what the budgets are set against, and no device case measures it:
      `tests/device/Measure/LatencyCases.swift` has no case that A/Bs a voxel
      verb with undo on against undo off — the two that touch undo only use it
      to reset outside the timing. A new case means a device-gate cycle, which
      this change does not run; a Mac number is not offered in its place. The
      16 bytes per journaled cell is `sizeof(VoxelGrid::SculptChange)` and is
      the same on arm64, so only the ratio is outstanding

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
- [x] 2.4 DECIDED: enabling mid-session STARTS AN EMPTY HISTORY and is never
      refused; a second enable keeps the history it has. Enumerated against
      an existing session:
      - steps before enabling: none recorded, depth 0 — the document as it
        stands is the starting state. Recording a barrier at the switch was
        rejected: nothing before it is reversible anyway, it would add a step
        to every SDF-only host (4.5), and a journal barrier at index 0 would
        stop every "load, enable, crash, replay" recovery before it began.
      - refusing when the document has edits was rejected: it changes what
        `enable_undo` means on the SDF path, and "has edits" is not
        detectable for four representations anyway (#641).
      - a partially-applied gesture: a bracket cannot straddle the switch —
        `begin_undo_group` is refused while off — but its END could, and an
        unmatched `end_group` folded every step since the session began into
        ONE. Fixed: an unmatched end is a no-op, and brackets are
        depth-counted so only the outermost folds (6.7, 6.8).
      - memory: enabling allocates the history and nothing else.
      - the crash journal: seeded with the last loaded/saved snapshot, which is
        wrong if the document was edited since — replay then silently lacks the
        pre-enable edits. Probed and filed as #641; documented workaround is to
        save once after enabling mid-session

## 3. Build

- [x] 3.0 A `session` module, and its line in `tools/check_layering.py`
- [x] 3.1 A second recording channel on `VoxelGrid::set`, independent of the
      sculpt-layer stack and written only when the history is enabled — plus a
      step-scoped revert/reapply beside the private `revert_from` / `apply_from`
      that already do the replay for sculpt layers
- [x] 3.2 The session history: an ordered log of steps, each naming its owner
      and carrying the token that reverses it
- [x] 3.3 Two voxel step kinds — the pass, and a change to a pass (strength,
      visibility, order, merge-down) — so undoing a strength tweak does not
      remove the pass. BUILT as `Step::Kind::VoxelLayerProperty` carrying a
      `VoxelGrid::SculptLayerOp`: the property AND every cell the recompose
      rewrote, replayed without recomposing, so undo is bit-exact by
      construction. Removal is a step too. The mesh stack's shape — an optional
      record out-parameter on each operation and one apply either way —
      reached through the existing `GridFor`, no new resolver
- [x] 3.4 Merge-down holds the folded record, since undoing it means restoring
      one. The only voxel step whose memory scales with the pass. BUILT holding
      the UPPER record plus the lower's prior length and the afters the fold
      overwrote — not a second copy of the lower layer, which is the one most
      likely to be large
- [x] 3.5 Redo discarded on the next edit, across representations
- [x] 3.7 (CORRECTED — not needed, see 6.4 and 6.5) Move history ownership onto `io::ClaySpaceDoc`, so the two bindings
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
- [x] 4.4 A voxel strength change undoes without removing its pass —
      `test_voxel_layer_history.cpp`, `test_c_voxel_layer_history.cpp`,
      `test_voxel_layer_history.py`, bit-exact on `serialize()` / the document
      bytes, undo AND redo, for all five operations
- [x] 4.5 A host that only ever edits SDF sees behaviour bit-identical to today

## 5. Reach it and say it

- [x] 5.1 C ABI — the existing undo entry points, now spanning three
      representations, plus whatever 2.2 decides
- [x] 5.2 pyclay, so `check_binding_parity` stays clean
- [x] 5.3 Swift smoke: a voxel sculpt-layer dial undone through
      `clay_document_undo` restores the slider
- [x] 5.4 ABI minor bump and `docs/RELEASE.md`, stating plainly that undo now
      reverses more than it did — a behaviour change and a fix
- [x] 5.5 `docs/05-claycore-library.md`: the history section, which did not
      exist, and which `correct-the-undo-scope` is the reason to write
- [x] 5.6 A numbered example that crosses representations and undoes back
- [x] 5.7 `openspec/ROADMAP.md`, and `correct-the-undo-scope` updated — it names
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

- [x] 6.5 A finding the pyclay wiring surfaced, in favour of the position 6.4
      argued against: `PyVoxelGrid` carries the `ClaySpaceDoc`, not the
      `PyDocument` that owns the history, so the handle had to be given a
      reference to the history explicitly. Had the history lived on
      `ClaySpaceDoc` — task 3.7 — no plumbing would have been needed in either
      binding. 6.4 is still right that ownership does not fix CALL-SITE drift;
      it was wrong that ownership buys nothing. Not worth reworking the C side
      for, and recorded so the next person does not have to rediscover it

- [x] 6.6 The undo journal read the previous cell value with a second `get(c)`
      hash probe on the write path. `write_cell` already reads it — it is the
      one place that has it — so it hands it back through an out-parameter now.
      Measured 1.26x -> 1.005x on `sculpt_smooth`

- [x] 6.7 CORRECTED while deciding 2.4: `History::end_group` with no bracket
      open folded every step since the last bracket's start into one. A host
      reaches it by enabling undo mid-gesture. Now a no-op; regression tests at
      the session, C and pyclay levels, proved against the old body
- [x] 6.8 CORRECTED: an inner `begin_group` moved the fold's start past what
      the outer bracket had already collected, so a nested bracket undid in
      two. Depth-counted now; regression test
- [x] 6.9 CORRECTED while reading the c-abi delta against the code: "a host
      moves a mesh layer's vertices and calls undo" was false at the ABI. A
      `clay_mesh_sculptor` stamp over a document mesh layer is not a document
      step — probed: 17 classes moved, depth unchanged, the next undo removed
      the layer. The scenario now states what the ABI records (replacements),
      pinned by a test; recording stamps is #643
- [x] 6.10 CORRECTED in the scene-model delta: its barrier examples were still
      consolidate and rasterize, which 6.1 had already shown are recorded
- [x] 6.11 FOUND: creating a voxel sculpt layer is not a history event, so an
      undone pass stays in its record and a journal replayed onto an older
      snapshot rebuilds cells and not the stack. Filed as #642

## 7. Still open after this slice

- [x] 7.1 CLOSED by 3.3 and 3.4. Sculpt-layer PROPERTY changes (strength, visibility, order) are not
      steps. Their cell effect is restorable by replay, but the property value
      is not, so an undo would restore the pixels and not the setting — a
      partial undo, which is worse than none. Needs the second voxel step kind
      (3.3) and the merge-down record retention (3.4)
- [x] 7.2 pyclay wired. The bindings agree about what undo means again
