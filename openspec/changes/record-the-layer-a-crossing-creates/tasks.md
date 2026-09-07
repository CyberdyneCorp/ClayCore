# Tasks

- [x] Route `clay_document_add_voxel_layer` through `AddLayerCmd` with a
      reserved id, and keep the grid in the side map with `insert_or_assign`.
- [x] The same for `pyclay`'s `Document.add_voxel_layer`, so the
      python-bindings undo claim is true rather than aspirational.
- [x] `Step::Kind::Compound`: fold a closed bracket into one step, Scene child
      first, with a refused child leaving the whole step unapplied.
- [x] Leave a bracket alone when it holds a barrier, and when it produced fewer
      than two steps.
- [x] Hold `enforce_budget` off while a bracket is open, and apply it when the
      fold closes.
- [x] Count Scene steps recursively in `sync_scene_steps`, and recurse in
      `step_bytes` so a folded group reports what its children cost.
- [x] Filter voxel chunks by their layer on save, and drop unmatched ones on
      load; tick `add-mesh-layers` task 7.7 for the voxel side (masks stay open).
- [x] Regression tests: creating a voxel layer is a step; a bracketed crossing
      is ONE step and redo restores layer and cells; an ungrouped crossing is
      still two, in order; an undone crossing saves nothing and its id is
      reusable; a command-only bracket is unchanged; a barrier is not swallowed;
      a refused child leaves the step unapplied.
- [x] Prove each test fails with the fix reverted.
- [x] Update `clay.h`'s undo block, which was stale on masks and silent on layer
      creation, and `docs/05-claycore-library.md`.
- [x] Bump CMakeLists.txt and pyproject.toml to 0.56.0.

## Why this reads 0 open tasks and is still not archived

Recorded 2026-09-06, after an archive attempt aborted, so the next person does
not spend the same half hour re-deriving it.

**The work shipped. The archive is blocked on a change that has not.** This
change's `scene-model` delta is a `MODIFIED` block against
`### Requirement: One undo order spans every representation`, and that
requirement is not in `openspec/specs/scene-model/spec.md`. It is `ADDED` by
`unify-the-undo-history`, which is still open with 9 tasks. So the ordering is
forced: `unify-the-undo-history` lands, and this archives after it.

Two ways to unblock it were considered and rejected:

- **Rewrite this delta to ADD the requirement.** It would then collide with
  `unify-the-undo-history`'s own ADD the moment that change archives, and the
  loser of that collision is whichever runs second — silently, because an ADD of
  an existing requirement is the failure the validator reports last.
- **Archive with `--skip-specs`.** The deltas are real; four capabilities
  genuinely change. Skipping them would put the change in the archive while its
  requirements never reached the living specs, which is the exact state that
  makes `openspec/specs/` describe a library the tree does not have.

**One thing WAS fixed here**, because it was a genuine defect rather than an
ordering problem: the `python-bindings` delta named
`### Requirement: Undo covers every reachable edit`, which has since been renamed
to `### Requirement: Undo from Python` and absorbed the general sentence about
every entry point recording its inverse. The block is refreshed against its live
base and repeats every scenario that requirement now carries, as a `MODIFIED`
block must under the 1.12.0 validator. Verified the underlying claim still holds:
`bindings/python/pyclay_module.cpp` routes `add_voxel_layer` through
`AddLayerCmd`, with the comment saying why.
