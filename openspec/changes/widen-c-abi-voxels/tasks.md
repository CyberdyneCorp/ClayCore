# Tasks: widen-c-abi-voxels

## 1. Voxel handles and edits
- [ ] 1.1 Opaque `clay_voxel_grid`; standalone create/destroy; borrowed document-layer handles with a protected destroy
- [ ] 1.2 Palette: add/get/set/size
- [ ] 1.3 Single and batch edits: set/erase/paint, set_many/erase_many, fill_box, fill_line, mirrored variants
- [ ] 1.4 `clay_brush_params` (versioned): size, shape, falloff, strength, seed; set/erase/paint brush
- [ ] 1.5 Sculpting verbs: smooth, inflate, flatten, pinch
- [ ] 1.6 Queries: get, occupied count, bounds, flood select (size-query), step field
- [ ] 1.7 Greedy mesh; rasterize from a document

## 2. Evaluation, picking, meshing
- [ ] 2.1 Gradients, colours, batch raycast, safe step scale
- [ ] 2.2 Surface snap; layer bounds; selection bounds; attributed raycast (layer + node)
- [ ] 2.3 Voxel cell/face pick and build-plane pick
- [ ] 2.4 Mesher selection in a versioned mesh-params struct, experimental gated

## 3. Parity gate
- [ ] 3.1 Gate enumerating pyclay's capability surface against the C entry points, with recorded exemptions
- [ ] 3.2 Extend the ctypes FFI exercise to the new surface
- [ ] 3.3 Swift smoke: build a document, sculpt voxels, mesh, through the xcframework

## 4. Close-out
- [ ] 4.1 Docs: `docs/05`, README capability table, `docs/RELEASE.md` open items
- [ ] 4.2 Full verification: all presets, gates, gallery, release checklist, CI
