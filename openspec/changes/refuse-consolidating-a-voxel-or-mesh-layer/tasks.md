## Implementation
- [x] Add `require_sdf_layer` in `bindings/c/clay_c.cpp` and call it after the not-found check in cost, consolidate_cancellable (and so consolidate), plan_region_merge and consolidate_region.
- [x] Leave `clay_layer_consolidation_advice` and `clay_layer_consolidation_state` answering a voxel or mesh layer as before.
- [x] Regression tests in `tests/unit/test_c_consolidate.cpp`: every refusing entry point on a voxel and a mesh layer answers `CLAY_ERROR_UNSUPPORTED`, names the representation, and leaves the undo depth unchanged. A locked grid is still unsupported, and the advice still answers. Control: an empty SDF layer still answers `CLAY_ERROR_INVALID_ARGUMENT`. The test fails on the pre-fix main (24 failed assertions).
- [x] Update the `clay.h` comments and `docs/05`.
- [x] No ABI entry-point, struct, format or version change: the ABI stays at 0.123.0.
