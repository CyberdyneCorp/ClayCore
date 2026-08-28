# Tasks

## 1. The accessors

- [x] 1.1 `clay_document_voxel_layer_by_id` borrows the grid of the voxel layer
      carrying the id
- [x] 1.2 `clay_document_mesh_layer_by_id` borrows the mesh of the mesh layer
      carrying the id
- [x] 1.3 Both resolve through `Document::find_layer` FIRST, so a layer whose
      creation is currently undone is not reachable through the grid the undo
      deliberately kept — the side table alone is not the lookup
- [x] 1.4 A layer of another representation is `CLAY_ERROR_NOT_FOUND`, not a
      borrow of the wrong kind
- [x] 1.5 A layer with no payload entry is `CLAY_ERROR_NOT_FOUND`, matching the
      by-name form's guard
- [x] 1.6 A null document, and a null out pointer, are
      `CLAY_ERROR_INVALID_ARGUMENT`; a refused call writes nothing

## 2. Decide, and say so in the header

- [x] 2.1 DECIDED: the out pointer is REQUIRED, unlike the by-name form. There
      the NULL is meaningful — the call doubles as an existence probe and still
      reports the id through `out_layer`. Here the caller supplied the id and
      the handle is the only answer, so a NULL out pointer asks nothing.
      `clay_document_layer_info` is the existence-and-representation query
- [x] 2.2 DECIDED: the by-name pair stays. It is the convenient call for a
      document with one layer of that name, its answer is documented, and
      removing it would break callers to buy a rule this ABI does not keep
- [x] 2.3 DECIDED: no new lifetime rule. A borrowed handle already names its
      layer and re-resolves on every call, so these hand back what the create
      calls hand back, on the same terms

## 3. Prose, which is what the issue is complaining about

- [x] 3.1 `clay_document_set_layer_name`: the advice to hold the id is now
      followable — name the two calls that follow it
- [x] 3.2 `clay_document_voxel_layer` and `clay_document_mesh_layer`: each names
      its by-id sibling as the lookup that survives a rename
- [x] 3.3 The two-lifetimes note above `clay_voxel_grid` enumerates the calls
      that return a borrowed grid — add the new one
- [x] 3.4 `docs/05-claycore-library.md`, `docs/07-brushes-and-features.md` and
      `docs/08-mesh-readback.md`, where the by-name lookups are listed and the
      shadowing is described. RELEASE.md keeps no per-minor entry for an
      additive release — 0.55.0 and 0.56.0 have none either — so it is left
      alone deliberately

## 4. Prove it

- [x] 4.1 Two voxel layers sharing one name: each reachable by its own id, while
      the by-name call reaches only the first in stack order
- [x] 4.2 The same for two mesh layers, including that the geometry differs, so
      "the right one" is checked rather than "a layer"
- [x] 4.3 Survives a save and reload: the ids still resolve after a round trip
      and still tell the two same-named layers apart
- [x] 4.4 A rename does not move what the id reaches
- [x] 4.5 Wrong representation: a mesh layer's id through the voxel accessor and
      an SDF layer's id through both are `CLAY_ERROR_NOT_FOUND`
- [x] 4.6 An unknown id is `CLAY_ERROR_NOT_FOUND`
- [x] 4.7 An undone creation: the id is not reachable even though the grid is
      still beside the document, and a redo makes it reachable again
- [x] 4.8 A layer whose payload chunk is absent from the file is
      `CLAY_ERROR_NOT_FOUND`, as it is by name
- [x] 4.9 Null document and null out pointer, with nothing written
- [x] 4.10 The C consumer smoke test and the ctypes parity exercise drive both
      calls
- [x] 4.11 Each property FAILS under a targeted mis-implementation that still
      COMPILES. Baseline: 7 cases, 118 assertions, all passing. Every revert
      built with zero `error:` lines before its run was believed.

      | revert (both accessors) | cases failed | assertions failed |
      |---|---|---|
      | A — resolve through the layer's NAME instead of its id | 2 of 7 (the two duplicate-name cases) | 7 of 118 |
      | B — resolve in the payload table alone, never in the document | 2 of 7 (undone creation, removed layer) | 4 of 118 |
      | C — no representation guard: any existing layer id borrows | 3 of 7 (both wrong-representation cases, payload absent) | 7 of 118 |
      | D — drop the payload-entry check | 1 of 7 (payload absent) | 2 of 118 |
      | E — drop the kind check alone | 0 of 7 | 0 of 118 |

      Revert A also fails the smoke test at smoke.c:1052 and the ctypes case
      `test_same_named_layers_are_told_apart_by_id`; revert C fails the smoke
      test at smoke.c:1053 and `test_mesh_layer_by_id_reaches_the_geometry_the_name_shadows`.

- [x] 4.12 RECORDED, because a revert that fails nothing is a result too: E is
      not observable. `voxel_layers` and `mesh_layers` are keyed by layer id and
      written by nothing but the matching create call, and the loader drops a
      payload whose layer is of another kind, so a layer of the wrong kind is
      never in the other table. The kind check stays — the by-name form checks
      it in the same position and the two are required to agree, and the
      coincidence is a property of the create calls rather than of the type —
      and the code says so where it is written rather than leaving a reader to
      rediscover it
