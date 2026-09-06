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
