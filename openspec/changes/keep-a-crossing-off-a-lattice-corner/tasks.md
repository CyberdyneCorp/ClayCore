## 1. The defect

- [x] 1.1 The brick mesher emitted 2,297 sliver triangles in 83,464 on the
      `gnarly_document` fixture, 1,006 on the probe document
- [x] 1.2 MEASURED: a host hid them by re-meshing the whole field per stroke —
      `clay_document_mesh` degrades 9.40x across 48 dabs against the brick
      path's 1.49x, 26.2 ms against 6.3 ms. The slivers cost a host the cull.
- [x] 1.3 MEASURED the origin rather than assuming it: sliver vertices sit a
      median 0.0178 voxels from the lattice against 0.2576 for every other
      vertex, so the crossing parameter was landing on an edge endpoint

## 2. The change

- [x] 2.1 `edge_t(f0, f1, guard)` clamps the crossing to `[guard, 1-guard]`
- [x] 2.2 `Builder` takes the guard as a parameter defaulting to 0, so the
      guarded and unguarded paths are distinguishable at the call site
- [x] 2.3 `mesh_bricks` and `shell_corner_lattice` pass 0.05; `mesh_lattice` and
      `mesh_lattice_parallel` pass nothing and stay bit-identical

## 3. Why the scope is narrow

- [x] 3.1 A global clamp was written first and broke 15 assertions across
      `test_mesh_weld.cpp`, `test_voxel_remesh.cpp`, `test_c_voxel_remesh.cpp` —
      fixtures that produce degenerates deliberately to exercise the welder
- [x] 3.2 It also made `decimate` return a non-manifold mesh from a valid
      watertight input; that is the decimator's fragility, filed as #567
- [x] 3.3 So the brick-only scope is a measured conclusion, not a precaution

## 4. Tests

- [x] 4.1 `tests/unit/test_mesh.cpp`: the brick mesher emits no sliver triangle,
      and the mesh stays watertight and manifold
- [x] 4.2 It fails without the guard — 2,297 slivers — so it pins the fix rather
      than restating a property the mesher already had
- [x] 4.3 It requires >1000 triangles first, so an empty mesh cannot pass it
      vacuously
- [x] 4.4 An earlier version of the test used `mesh_tape` and measured 234
      slivers. That was correct for the tape path, which is deliberately
      unguarded, and said nothing at all about this one. The test meshes through
      `mesh_bricks`.

## 5. Still open

- [ ] 5.1 A host that pins rendered images needs to refresh them; told before
      this landed rather than after
- [ ] 5.2 #567: `decimate` can return a non-manifold mesh from a valid input
- [ ] 5.3 Device gate on the reference iPad before the tag that carries this
