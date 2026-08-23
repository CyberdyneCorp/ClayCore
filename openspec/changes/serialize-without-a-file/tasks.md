# Tasks: serialize-without-a-file

## 1. Measure before designing

- [x] 1.1 Confirm the asymmetry is systematic, not one omission: `clayspace`,
      `obj`, `ply`, `fbx`, `glb` and `mesh_stream` all have buffer forms in
      `include/clay/io/`, and the C ABI exposes only `clay_document_save/load`
      and `clay_mesh_save/load`, all path-only
- [x] 1.2 Confirm the file forms are wrappers: `save_obj_file` is
      `write_whole_file(path, save_obj(...))`, so exposing the buffer form adds
      no second implementation to keep in step
- [x] 1.3 Confirm the return shape: variable-size payloads already cross as an
      opaque owner handle with borrowed accessors and an explicit free
      (`clay_mesh`, `clay_tape`). The size-query pattern would serialize twice
- [x] 1.4 Confirm what the OBJ sidecar does: `save_obj_file(with_mtl=true)`
      writes a companion `.mtl` and puts its NAME in the `mtllib` line, so the
      memory form must pass an empty name and emit no line

## 2. Build

- [x] 2.1 `clay_blob` — opaque, `clay_blob_data` / `clay_blob_size` borrowing,
      `clay_blob_destroy` freeing. One type for every serialized payload
- [x] 2.2 `clay_document_save_memory` / `clay_document_load_memory`, the latter
      taking NO budget, because the only one the path loader uses is the
      file-byte ceiling and a caller holding a buffer has already read it
- [x] 2.3 `clay_mesh_save_memory` / `clay_mesh_load_memory`, format by NAME,
      matched case-insensitively, unknown name refused rather than defaulted
- [x] 2.4 The import budget on the mesh memory loader, enforced identically
- [x] 2.5 pyclay: `Document.to_bytes()`, `clay.load_bytes()`,
      `Mesh.to_bytes(format)`, `clay.load_mesh_bytes(...)`

## 3. Prove it

- [x] 3.1 The scenarios in both spec deltas
- [x] 3.2 The equivalence test, which is what makes this plumbing rather than a
      second format: save to a path and to memory, compare the bytes
- [x] 3.3 A document round trip through memory that evaluates identically
- [x] 3.4 Every format round-trips through memory
- [x] 3.5 The refusals: null pointer, zero length, unknown format name, a
      truncated buffer, and a budget smaller than the mesh
- [x] 3.6 The borrowed bytes survive an edit to the object they came from

## 4. Reach it and say it

- [x] 4.1 ABI minor bump and `docs/RELEASE.md`
- [x] 4.2 `docs/08-mesh-readback.md` and the parity table in `docs/07`
- [x] 4.3 `openspec/ROADMAP.md`: record the finding, since the roadmap did not
      have this row either
