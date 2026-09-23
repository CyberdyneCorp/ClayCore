# Tasks: make-voxel-sculpt-layer-creation-a-step

## 1. Reproduce

- [x] 1.1 On `main`, through the C ABI: undo a dab inside a layer and read the
      layer's cell count (110, not 0); dial the layer and count occupied cells
      (54 undone cells put back); replay a journal onto a snapshot older than
      the layer (refused after 2 of 3 events)

## 2. Build

- [x] 2.1 `SculptLayerOp::Kind::Begin` and `Kind::Pass`, appended; `pass_afters`
      in the struct and the journal encoding; `op_fits` checks both
- [x] 2.2 `begin_sculpt_layer(name, record)`; the C and Python bindings record it
- [x] 2.3 `VoxelGrid::begin_pass_capture` / `end_pass_capture`, fed by the
      recording hook's rewrite branch
- [x] 2.4 `History::begin_voxel_step` / `end_voxel_step` open and close the
      capture; a changed record makes the step a `VoxelLayerProperty` Pass; a
      step with no changed cell rolls its record entries back
- [x] 2.5 The pinned journal test now snapshots BEFORE the layers exist

## 3. Verify

- [x] 3.1 Regression tests, failing on `main` and passing here:
      `test_c_voxel_layer_history.cpp` (three cases) and
      `test_voxel_layer_history.cpp` (five cases plus the Pass encoding) and
      `bindings/python/tests/test_voxel_layer_history.py` (one case)
- [x] 3.2 The rollback's own regression fails with the rollback removed
- [x] 3.3 Full unit suite; `release_check.py --skip-slow`

## 4. Document

- [x] 4.1 `bindings/c/clay.h` undo notes; `docs/05` "What is NOT covered"
- [x] 4.2 No ABI version move: no entry point added, no signature changed
