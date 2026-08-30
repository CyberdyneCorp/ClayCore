# Tasks: remesh-through-the-document

## 1. History

- [x] 1.1 `Step::Kind::MeshReplace` with both meshes as payload
- [x] 1.2 `record_mesh_replace`, dropping a replacement that changed nothing
- [x] 1.3 `apply_step` case, copying rather than moving so redo can reuse it
- [x] 1.4 `step_bytes` term, so the budget can see the largest kind there is
- [x] 1.5 A journal kind, APPENDED, with a self-contained mesh codec — `session`
      may not name `io`, and a private in-session encoding needs no version

## 2. The revision

- [x] 2.1 Per-layer geometry revision on the C document handle and on `PyDocument`
- [x] 2.2 Bumped by a wholesale replacement and never by a sculpt
- [x] 2.3 Checked by `resolve_sculptor` and by `PyMeshSculptor::live`
- [x] 2.4 A regression test that only the revision check can satisfy: same
      counts, reversed indices — proven by reverting the check

## 3. The commands

- [x] 3.1 One internal replacement helper per binding, holding the guards, the
      undo record and the invalidation
- [x] 3.2 `clay_document_voxel_remesh_layer`, transactional, protected layers
      refused before the rebuild
- [x] 3.3 `clay_document_replace_mesh_layer` with an expected revision
- [x] 3.4 `clay_document_mesh_layer_revision`
- [x] 3.5 The same three on `pyclay.Document`
- [x] 3.6 Undo guarded — it is opt-in, and the first version dereferenced it
      unconditionally

## 4. Report fields

- [x] 4.1 One-sided result-to-source distance: RMS, p95, max
- [x] 4.2 Per-stage wall clock
- [x] 4.3 The memory figure the guard compared, named as an estimate and not a peak
- [x] 4.4 Carried across the C ABI and pyclay through one shared dict builder

## 5. Projection

- [x] 5.1 Measure the hard reject against no test and against a continuous weight
- [x] 5.2 Replace the reject with the weight
- [x] 5.3 A test on the fold that asserts the branch is exercised AND that
      projection adds no self-intersection

## 6. Fixtures and coverage

- [x] 6.1 `folded_sheet` — a self-intersection inside one surface
- [x] 6.2 `chamfered_cube` — the fixture that tells the surface modes apart
- [x] 6.3 `noisy_sphere` — a million triangles
- [x] 6.4 Sharp mode measured against smooth on the chamfer
- [x] 6.5 A `DynamicSurface` round trip, with the weld epsilon it needs
- [x] 6.6 The refused-request allocation gate, in the one TU that may replace
      `operator new`

## 7. Performance

- [x] 7.1 Extraction: a per-brick fast path for the empty majority, and a march
      restricted to the stored bricks' own extent
- [x] 7.2 Proven output-identical by the existing sparse/dense byte comparison
- [x] 7.3 The next lever measured and recorded rather than half-taken

## 8. Repository

- [x] 8.1 Version lines moved together
- [x] 8.2 Docs
- [x] 8.3 `openspec validate --strict` clean
