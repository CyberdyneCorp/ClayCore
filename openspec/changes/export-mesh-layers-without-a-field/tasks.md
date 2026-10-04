## Implementation
- [x] `tests/unit/test_c_mesh_layer.cpp`: a hidden SDF layer, no SDF item at all, and every surface group hidden each export the placed mesh layer alone (4 vertices, 12 indices, bounds under the layer transform); nothing visible is still `CLAY_ERROR_INVALID_ARGUMENT`; a malformed parameter block is refused with a mesh layer present.
- [x] Proved the three export cases fail on the pre-fix code (`CLAY_ERROR_INVALID_ARGUMENT` twice, `CLAY_ERROR_BACKEND` once).
- [x] `bindings/c/clay_c.cpp`: `place_mesh_layer` / `place_visible_mesh_layers` pulled out of `clay_document_mesh_combined`; `mesh_tape` takes an `EmptyField` mode, and its voxel-size pricing moves unchanged into `price_mesh_voxel` so `mesh_tape` stays at cognitive complexity 14 (it was 15 on main; the two empty checks alone took it to 19).
- [x] `bindings/c/clay.h`: the combined call's comment states that an empty field contributes nothing.
- [x] `docs/08-mesh-readback.md`: the "field + visible mesh layers" row.
- [x] No ABI version change (stays 0.124.0): no signature or entry point moves.
- [x] `ctest --preset cpu-only` and `tools/release_check.py --skip-slow`.
