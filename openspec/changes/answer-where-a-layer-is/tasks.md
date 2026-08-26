# Tasks

- [x] 1.1 `clay_layer_bounds` answers from a voxel layer's occupied cells, with a cell treated as a box rather than a point
- [x] 1.2 `clay_layer_bounds` answers from a mesh layer's vertices
- [x] 1.3 Both compose with the layer transform, so every arm answers in world space
- [x] 1.4 A layer with no material still reports `has_bounds == 0`
- [x] 1.5 `pick::layer_bounds` is unchanged and the layering gate still passes
- [x] 1.6 Rewrite the test that asserted the old behaviour, recording why it reverses
- [x] 1.7 Test: a mesh layer's bounds equal `clay_mesh_bounds` on the same geometry
- [x] 1.8 Test: a voxel layer's bounds follow the occupied cells, including the one-cell case
- [x] 1.9 Test: the blocked conversion — a mesh layer's bounds drive `clay_voxel_rasterize_mesh`
- [x] 1.10 Verify every new test FAILS with the fix reverted
- [x] 1.11 Update the `clay.h` contract
