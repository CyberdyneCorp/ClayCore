# Tasks: widen-c-abi

## 1. Foundations
- [ ] 1.1 `uint32_t struct_size` convention on all descriptor structs; teach `tools/check_c_abi.py` to require it
- [ ] 1.2 Bump ABI to 0.2.0 in `clay.h`, `CMakeLists.txt`, `pyproject.toml` (the release gate checks agreement)
- [ ] 1.3 Enum sync: `clay_prim` values equal tape opcodes; a static_assert per primitive/op/blend so drift is a compile error

## 2. Item builder
- [ ] 2.1 `clay_item_create`/`_destroy`; setters for transform, op, blend, rounding, colour, mirror
- [ ] 2.2 Deformer chain setter preserving order; repetition setter
- [ ] 2.3 Profile setter incl. polygon vertices; stroke point list; transition parameters
- [ ] 2.4 `clay_layer_add_item`; redefine flat `clay_add_item` as sugar over the builder
- [ ] 2.5 Tests: composed edit equals the pyclay-authored document; deformer order; flat path unchanged

## 3. Voxels
- [ ] 3.1 Opaque `clay_voxel_grid`; standalone create/destroy plus borrowed document-layer handles with a protected destroy
- [ ] 3.2 Palette: add/get/set/size
- [ ] 3.3 Single and batch edits: set/erase/paint, set_many/erase_many, fill_box, fill_line, mirrored variants
- [ ] 3.4 `clay_brush_params` (size, shape, falloff, strength, seed); set/erase/paint brush
- [ ] 3.5 Sculpting verbs: smooth, inflate, flatten, pinch
- [ ] 3.6 Queries: get, occupied count, bounds, flood select (size-query), step field
- [ ] 3.7 Greedy mesh; `rasterize` from a document
- [ ] 3.8 Tests incl. the borrowed-handle destroy guard and C-vs-pyclay mesh equality

## 4. Evaluation, picking, meshing
- [ ] 4.1 Gradients, colours, batch raycast, safe step scale
- [ ] 4.2 Surface snap; layer bounds; selection bounds; attributed raycast (layer + node)
- [ ] 4.3 Voxel cell/face pick and build-plane pick
- [ ] 4.4 Mesher selection in a versioned mesh-params struct (marching, nets, dual contouring + experimental flag)

## 5. Parity gate
- [ ] 5.1 A gate that enumerates pyclay's capability surface and fails on a C entry point that is missing without a recorded exemption
- [ ] 5.2 Extend the ctypes FFI exercise to the new surface
- [ ] 5.3 Swift smoke: build a document, sculpt voxels, mesh, through the xcframework

## 6. Close-out
- [ ] 6.1 Docs: `docs/05`, README capability table, `docs/RELEASE.md` open items
- [ ] 6.2 Full verification: all presets, gates, gallery, release checklist, CI
