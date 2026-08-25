# Tasks: report-mesh-quality

## 1. Measure before designing

- [x] 1.1 Confirm what is discarded: `mesh::ValidationReport` carries eleven
      fields; `clay_mesh_validate` returns two `int32_t`. Nine measured
      quantities never cross
- [x] 1.2 Confirm the self-intersection pass is unreachable from a binding —
      neither `clay_mesh_validate` nor pyclay passes `max_intersection_pairs`,
      so both take the default of 0, which skips it. The only callers that ever
      pass one are `tests/unit/test_mesh.cpp` (20000 and 5000)
- [x] 1.3 Confirm `clean()` overclaims through that gap: it requires
      `intersecting_pairs == 0`, which is what an unrun pass leaves behind
- [x] 1.4 Confirm `mesh::signed_volume` / `mesh::surface_area` reach neither
      binding, and that there is no volume or area query anywhere else in the
      public surface
- [x] 1.5 Confirm pyclay runs a full `validate()` per predicate, so asking both
      `is_watertight()` and `is_manifold()` builds the edge map twice

## 2. Build

- [x] 2.1 `clay_validation_report` with `struct_size`, and its original-layout
      constant named by its last field so appending one cannot move the
      baseline
- [x] 2.2 `clay_mesh_validation_report`, taking the self-intersection cap,
      reading the incoming `struct_size` with `read_desc` and filling with
      `write_desc`
- [x] 2.3 Echo the cap in the report, so an unrun pass is distinguishable from
      a clean one
- [x] 2.4 `clay_mesh_measure` for signed volume and surface area, in the
      out-parameter shape `clay_mesh_bounds` uses
- [x] 2.5 Redefine `clay_mesh_validate` as sugar over the report rather than
      leaving a second call path to the same validator
- [x] 2.6 pyclay: `Mesh.validation_report(...)` as a dict, matching
      `Mesh.quad_report`; `signed_volume` and `surface_area`. The two existing
      predicates are left running their own validation — caching a report on a
      mesh a `MeshSculptor` can edit would go stale, and the fix for asking two
      questions is one call that answers both, not a cache

## 3. Prove it

- [x] 3.1 The scenarios in both spec deltas
- [x] 3.2 The regression for the defect itself: a mesh with a deleted triangle
      reports a non-zero boundary-edge count through the ABI, which is the
      number the meshing scenario has always said it reports
- [x] 3.3 A self-intersecting mesh is caught with a cap and missed without one,
      and the report says which happened
- [x] 3.4 `clay_mesh_validate` returns exactly what it returned before
- [x] 3.5 An older `struct_size` is honoured: a short declared prefix is filled
      and nothing past it is touched

## 4. Reach it and say it

- [x] 4.1 ABI minor bump and `docs/RELEASE.md`
- [x] 4.2 The parity table in `docs/07-brushes-and-features.md`
- [x] 4.3 `docs/08-mesh-readback.md`, which is where a host looks for what it
      can learn about a mesh it just got back
- [x] 4.4 `openspec/ROADMAP.md`: record that the deferred "output descriptors"
      family had a sibling — a report that was bounded correctly and simply
      never carried its fields
