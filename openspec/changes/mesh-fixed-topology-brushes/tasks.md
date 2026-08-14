# Tasks: fixed-topology mesh brushes

## 1. Adjacency

- [x] 1.1 `mesh/adjacency.h` + `src/mesh/adjacency.cpp`: `Adjacency::build(mesh, weld_epsilon)`
      building weld classes (position hash, epsilon relative to the bbox
      diagonal), the class→members CSR, the class→ring CSR and the
      vertex→triangle CSR, in one pass.
- [x] 1.2 `Adjacency::matches(const Mesh&)` by vertex and index count; every
      entry point taking both checks it.
- [x] 1.3 `geodesic_region` — Dijkstra over the class graph bounded by BOTH the
      brush's ball and a path budget, ties broken on class index, scratch reused
      across calls.
- [x] 1.4 Header states what the walk approximates and what it does not: edge
      paths overestimate geodesic distance, which is why the ball bounds the
      region and the straight line weighs it.

## 2. The verbs

- [x] 2.1 `mesh/sculpt.h`: `MeshFalloff` (own enum — `mesh` may not include
      `voxel`, and the header says why), `MeshBrush`, `MeshBrushSettings`,
      `BrushRegion`, `VertexDeltas`, `MeshSculptor`.
- [x] 2.2 `gather_region` — the pre-stamp snapshot: classes, weights, positions,
      normals, average normal, centroid, and the plane those two define.
- [x] 2.3 M1: grab, draw, inflate, smooth, pinch (signed), flatten (three modes).
- [x] 2.4 M2: clay, crease, scrape, polish, snakehook.
- [x] 2.5 `recompute_normals` over the touched classes plus their ring,
      angle-weighted; no-op on a mesh carrying no normals.
- [x] 2.6 `VertexDeltas`: sparse, coalesced per gesture, positions AND normals,
      `revert`/`apply` bit-exact and idempotent.
- [x] 2.7 `MeshSculptor` owns adjacency, BVH and scratch; `refresh_bvh()`.

## 3. Picking

- [x] 3.1 `Bvh` retains the source triangle index of each partitioned triangle;
      distance and winding results unchanged.
- [x] 3.2 `Bvh::raycast` — front-to-back traversal, Möller–Trumbore, no
      back-face culling.
- [x] 3.3 `pick::raycast_mesh` with the layer transform and shading-normal
      interpolation, falling back to the geometric normal.

## 4. The stroke engine's fourth consumer

- [x] 4.1 `brush::apply_to_mesh(MeshSculptor&, stamps, verb, settings, mask,
      deltas)` — per-stamp radius and strength from the stamp, mask per vertex,
      grab/snakehook deltas from the motion between stamps.
- [x] 4.2 Buildup vs clamped reaches the mesh through the stamp strengths
      `resolve_stroke` already produces; no accumulation logic here.

## 5. Tests

- [x] 5.1 Topology invariance: `indices` and `quads` byte-identical after every
      verb, on a quad-exported mesh.
- [x] 5.2 Determinism: same mesh, same stroke, bit-identical positions, twice.
- [x] 5.3 Seam: a welded adjacency crosses a UV seam and smoothing leaves no
      crack.
- [x] 5.4 Geodesic: a grab on the upper lip of a closed-mouth surrogate does not
      move the chin; the Euclidean falloff does.
- [x] 5.5 Mask: half-masked region under one stroke, for a displacement verb and
      for smooth; masked vertices bit-identical.
- [x] 5.6 Undo: bit-exact revert including normals; coalescing bounded by
      vertices reached; idempotent revert/apply.
- [x] 5.7 Verb behaviour: draw ≠ inflate on a saddle; pinch signed; flatten's
      three modes; clay's flat top; crease's fold; polish keeps a dihedral a
      plain smooth destroys; scrape ≠ flatten-then-smooth.
- [x] 5.8 Picking: ray names a triangle, barycentrics reconstruct the position,
      transform round trip, no-normals fallback, miss reports a miss.
- [x] 5.9 The document's evaluated field is unchanged by sculpting a mesh layer.
- [x] 5.10 Regression: `Bvh` distance and winding identical to `main` on a
      fixture mesh.

## 6. C ABI

- [x] 6.1 `clay_mesh_sculptor` handle: create/destroy/counts/refresh.
- [x] 6.2 `clay_mesh_sculpt_desc` versioned descriptor; unknown verb, falloff and
      flatten mode refused; non-positive radius refused; iteration cap bounded.
- [x] 6.3 `clay_mesh_sculptor_stamp` and `clay_mesh_sculptor_stroke` with the
      existing stroke preset descriptor and sample type, plus the mask handle.
- [x] 6.4 `clay_mesh_deltas` handle: create/destroy/count/revert/apply/clear.
- [x] 6.5 `clay_mesh_sculptor_raycast`.
- [x] 6.6 C tests under `tests/unit/test_c_mesh_sculpt.cpp`.

## 7. pyclay

- [x] 7.1 `MeshSculptor`, `VertexDeltas`, `MeshBrush`, `MeshFalloff` and the
      settings keywords; borrowed-mesh and protected-layer rules.
- [x] 7.2 `tools/check_binding_parity.py` passes; any exemption states a reason.

## 8. Docs and examples

- [x] 8.1 `docs/07-brushes-and-features.md` gains a § for mesh brushes.
- [x] 8.2 `docs/sculpt_comparison.md`'s non-goal amended to "topology-CHANGING
      sculpting", cross-referenced from `docs/07`, not deleted.
- [x] 8.3 `docs/08-mesh-readback.md` cross-reference: a mesh layer is now
      editable in place.
- [x] 8.4 `openspec/ROADMAP.md` row.
- [x] 8.5 Examples with committed renders: M1 on an import and on a quad
      re-import, each M2 verb, geodesic falloff, masking, undo. Counts printed.
- [x] 8.6 `examples/README.md` rows.

## 9. Validation

- [x] 9.1 `cmake --build` clean with `CLAY_WERROR=ON`; unit suite green.
- [x] 9.2 asan/ubsan preset green over the new tests.
- [x] 9.3 The gallery runs end to end and the renders are inspected.
- [x] 9.4 Cognitive complexity of the new functions checked against the
      systems-code band.


## 10. What the implementation changed about the plan

Recorded because two of these were found by looking at a RENDER, not by a test,
and the tests that now hold them were written afterwards.

- [x] 10.1 **The walk bounds the region; the straight line weighs it.** The
      design said "bounded Dijkstra, weight from the walk's distance". Weighing
      by path length bands visibly — an edge path overestimates geodesic
      distance by a direction-dependent amount — and bounding by path length
      alone leaves a ragged rim, because the same overestimate stops the walk
      short in some directions and not others. The region is now bounded by the
      brush's BALL and by a path budget, and the weight is the straight-line
      falloff with a taper over the budget's last stretch.
- [x] 10.2 **`clay` is a fill-only flatten onto a floating plane**, not "draw
      then clamp". Same result, and it says what the brush is.
- [x] 10.3 **The region's plane is its weighted centroid and average normal**,
      not a least-squares fit. Better for a curved patch, which is the case
      that matters, and it needs no eigensolver.
- [x] 10.4 **`polish`'s gate reads neighbouring CLASS normals, meaned, then
      spread by one ring and feathered by two.** Per-FACE normals cannot tell
      noise from a feature on exactly the surface polish is for; an unspread
      gate leaves a bead of untouched vertices along everything it protected;
      an unfeathered one leaves a step. The default angle is 0.20, not 0.35 —
      a loose gate is a plain smooth under another name.
- [x] 10.5 **`snakehook` re-anchors on the class it is dragging**, not on the
      cursor. Anchored on the cursor the surface falls behind by the falloff's
      weight each stamp and the brush walks out of its own radius; the tendril
      stops growing exactly when the pull gets interesting.
- [x] 10.6 **Class normals are ANGLE-weighted, not area-weighted.** A
      lattice-derived mesh has triangles of wildly uneven area, and an
      area-weighted normal varies at the lattice's frequency rather than the
      surface's — which `inflate` turns into a golf-ball dimple.
