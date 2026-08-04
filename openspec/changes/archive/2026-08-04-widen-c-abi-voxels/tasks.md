# Tasks: widen-c-abi-voxels

## 1. Voxel handles and edits
- [x] 1.1 Opaque `clay_voxel_grid`; standalone create/destroy; borrowed document-layer handles with a protected destroy
- [x] 1.2 Palette: add/get/set/size
- [x] 1.3 Single and batch edits: set/erase/paint, set_many/erase_many, fill_box, fill_line, mirrored variants
- [x] 1.4 `clay_brush_params` (versioned): size, shape, falloff, strength, seed; set/erase/paint brush
- [x] 1.5 Sculpting verbs: smooth, inflate, flatten, pinch
- [x] 1.6 Queries: get, occupied count, bounds, flood select (size-query), step field
- [x] 1.7 Greedy mesh; rasterize from a document

## 2. Evaluation, picking, meshing
- [x] 2.1 Gradients, colours, batch raycast, safe step scale
- [x] 2.2 Surface snap; layer bounds; selection bounds; attributed raycast (layer + node)
- [x] 2.3 Voxel cell/face pick and build-plane pick
- [x] 2.4 Mesher selection in a versioned mesh-params struct, experimental gated

## 3. Parity gate
- [x] 3.1 Gate enumerating pyclay's capability surface against the C entry points, with recorded exemptions
- [x] 3.2 Extend the ctypes FFI exercise to the new surface
- [x] 3.3 Swift smoke: build a document, sculpt voxels, mesh, through the xcframework
- [x] 3.4 Tests for every scenario in the delta: the sculpting sequence cell for cell and buffer for buffer, the borrowed handle, the flood-select size query, the falloff dither, selection bounds, and the experimental mesher's gate — through C against the engine and through ctypes against pyclay

## 4. Close-out
- [x] 4.1 Docs: `docs/05`, README capability table, `docs/RELEASE.md` open items
- [x] 4.2 Full verification: all presets, gates, gallery, release checklist, CI
