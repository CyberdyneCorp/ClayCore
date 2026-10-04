## Implementation
- [x] Regression test first: `tests/unit/test_move_topological.cpp` "a drag longer than the reach does not fold (#657)" reproduces the issue's probe and failed on the unfixed tree: 14 of 24 assertions, with the anchor at 0.939.
- [x] `ease_max_slope` moved to `include/clay/math/ease_slope.h`. `scene::ease_max_slope` is a using-declaration of it.
- [x] `field::move_topological` runs a drag as `topological_move_steps` slices (`MoveChain`): each slice's geodesic is solved over the source read through the earlier slices' composed pull-backs, and the output reads the source once per point. Both the callable and the `PointBatch` overloads.
- [x] `field::topological_move_steps` is public: half the fold limit, capped at 64.
- [x] C ABI 0.121.0 -> 0.122.0: `clay_item_volume_move_topological_from`. The descriptor reading is shared with the in-place form (`read_topological_move_settings`), and both refuse a non-finite anchor or displacement. Version moved in `CMakeLists.txt`, `bindings/c/clay.h` and `pyproject.toml`.
- [x] `check_binding_parity.py`: `Volume.moved_topologically_from` maps to `clay_item_volume_move_topological_from`.
- [x] Tests: slice-count rule; equivalence with n host calls (within 0.01); C `_from` against bake-then-move for a short drag (within 0.01 at cell 0.02); the issue's long drag through `_from` with default band and region; refusals.
- [x] Mutation check: forcing a single step (`kMaxSteps = 1`) fails both engine tests and the C long-drag case. Removing the defaulted-region growth fails the C long-drag case.
- [x] `docs/07`, `clay.h`, `field/move_topological.h` and the pyclay docstring state the sub-stepping and the new entry point.
