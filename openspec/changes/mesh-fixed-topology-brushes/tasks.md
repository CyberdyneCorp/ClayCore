# Tasks: fixed-topology mesh brushes

## 1. Adjacency

- [ ] 1.1 `mesh/adjacency.h` + `src/mesh/adjacency.cpp`: `Adjacency::build(mesh, weld_epsilon)`
      building weld classes (position hash, epsilon relative to the bbox
      diagonal), the class→members CSR, the class→ring CSR and the
      vertex→triangle CSR, in one pass.
- [ ] 1.2 `Adjacency::matches(const Mesh&)` by vertex and index count; every
      entry point taking both checks it.
- [ ] 1.3 `geodesic_region` — bounded Dijkstra over the class graph, ties broken
      on class index, scratch reused across calls.
- [ ] 1.4 Header states what the walk approximates and what it does not: edge
      paths overestimate geodesic distance; it is a falloff, not a measurement.

## 2. The verbs

- [ ] 2.1 `mesh/sculpt.h`: `MeshFalloff` (own enum — `mesh` may not include
      `voxel`, and the header says why), `MeshBrush`, `MeshBrushSettings`,
      `BrushRegion`, `VertexDeltas`, `MeshSculptor`.
- [ ] 2.2 `gather_region` — the pre-stamp snapshot: classes, weights, positions,
      normals, average normal, centroid, best-fit plane.
- [ ] 2.3 M1: grab, draw, inflate, smooth, pinch (signed), flatten (three modes).
- [ ] 2.4 M2: clay, crease, scrape, polish, snakehook.
- [ ] 2.5 `recompute_normals` over the touched classes plus their ring,
      area-weighted; no-op on a mesh carrying no normals.
- [ ] 2.6 `VertexDeltas`: sparse, coalesced per gesture, positions AND normals,
      `revert`/`apply` bit-exact and idempotent.
- [ ] 2.7 `MeshSculptor` owns adjacency, BVH and scratch; `refresh_bvh()`.

## 3. Picking

- [ ] 3.1 `Bvh` retains the source triangle index of each partitioned triangle;
      distance and winding results unchanged.
- [ ] 3.2 `Bvh::raycast` — front-to-back traversal, Möller–Trumbore, no
      back-face culling.
- [ ] 3.3 `pick::raycast_mesh` with the layer transform and shading-normal
      interpolation, falling back to the geometric normal.

## 4. The stroke engine's fourth consumer

- [ ] 4.1 `brush::apply_to_mesh(MeshSculptor&, stamps, verb, settings, mask,
      deltas)` — per-stamp radius and strength from the stamp, mask per vertex,
      grab/snakehook deltas from the motion between stamps.
- [ ] 4.2 Buildup vs clamped reaches the mesh through the stamp strengths
      `resolve_stroke` already produces; no accumulation logic here.

## 5. Tests

- [ ] 5.1 Topology invariance: `indices` and `quads` byte-identical after every
      verb, on a quad-exported mesh.
- [ ] 5.2 Determinism: same mesh, same stroke, bit-identical positions, twice.
- [ ] 5.3 Seam: a welded adjacency crosses a UV seam and smoothing leaves no
      crack.
- [ ] 5.4 Geodesic: a grab on the upper lip of a closed-mouth surrogate does not
      move the chin; the Euclidean falloff does.
- [ ] 5.5 Mask: half-masked region under one stroke, for a displacement verb and
      for smooth; masked vertices bit-identical.
- [ ] 5.6 Undo: bit-exact revert including normals; coalescing bounded by
      vertices reached; idempotent revert/apply.
- [ ] 5.7 Verb behaviour: draw ≠ inflate on a saddle; pinch signed; flatten's
      three modes; clay's flat top; crease's fold; polish keeps a dihedral a
      plain smooth destroys; scrape ≠ flatten-then-smooth.
- [ ] 5.8 Picking: ray names a triangle, barycentrics reconstruct the position,
      transform round trip, no-normals fallback, miss reports a miss.
- [ ] 5.9 The document's evaluated field is unchanged by sculpting a mesh layer.
- [ ] 5.10 Regression: `Bvh` distance and winding identical to `main` on a
      fixture mesh.

## 6. C ABI

- [ ] 6.1 `clay_mesh_sculptor` handle: create/destroy/counts/refresh.
- [ ] 6.2 `clay_mesh_sculpt_desc` versioned descriptor; unknown verb, falloff and
      flatten mode refused; non-positive radius refused; iteration cap bounded.
- [ ] 6.3 `clay_mesh_sculptor_stamp` and `clay_mesh_sculptor_stroke` with the
      existing stroke preset descriptor and sample type, plus the mask handle.
- [ ] 6.4 `clay_mesh_deltas` handle: create/destroy/count/revert/apply/clear.
- [ ] 6.5 `clay_mesh_sculptor_raycast`.
- [ ] 6.6 C tests under `tests/unit/test_c_mesh_sculpt.cpp`.

## 7. pyclay

- [ ] 7.1 `MeshSculptor`, `VertexDeltas`, `MeshBrush`, `MeshFalloff` and the
      settings keywords; borrowed-mesh and protected-layer rules.
- [ ] 7.2 `tools/check_binding_parity.py` passes; any exemption states a reason.

## 8. Docs and examples

- [ ] 8.1 `docs/07-brushes-and-features.md` gains a § for mesh brushes.
- [ ] 8.2 `docs/sculpt_comparison.md`'s non-goal amended to "topology-CHANGING
      sculpting", cross-referenced from `docs/07`, not deleted.
- [ ] 8.3 `docs/08-mesh-readback.md` cross-reference: a mesh layer is now
      editable in place.
- [ ] 8.4 `openspec/ROADMAP.md` row.
- [ ] 8.5 Examples with committed renders: M1 on an import and on a quad
      re-import, each M2 verb, geodesic falloff, masking, undo. Counts printed.
- [ ] 8.6 `examples/README.md` rows.

## 9. Validation

- [ ] 9.1 `cmake --build` clean with `CLAY_WERROR=ON`; unit suite green.
- [ ] 9.2 asan/ubsan preset green over the new tests.
- [ ] 9.3 The gallery runs end to end and the renders are inspected.
- [ ] 9.4 Cognitive complexity of the new functions checked against the
      systems-code band.
