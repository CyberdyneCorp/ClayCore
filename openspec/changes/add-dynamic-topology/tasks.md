# Tasks: add-dynamic-topology

- [x] 0.1 SEQUENCING (see ROADMAP, "Phase 5 — the surface tier"): after
      `add-shared-brush-kernels`, whose extracted kernels this consumes.
      `add-mesh-multires` follows it and shares its chunk runtime; the two
      SHALL NOT be built in parallel by different hands, because the chunk unit
      is the thing they share

## 1. Decide first

- [x] 1.1 DECIDE who owns a dynamic surface — a mesh layer with a new payload
      kind, or the host with `mesh::Mesh` at the boundaries. Recommendation:
      engine types now, document ownership as its own change, so this change
      does not carry a format decision it cannot yet justify
- [x] 1.2 DECIDE the determinism contract and write it into the requirement:
      the candidate set is ordered by stable id before any operator runs, so
      the same input produces the same sequence of operations on every platform
      and every standard library. Assuming single-threading supplies an order
      is how this promise breaks quietly
- [x] 1.3 DECIDE what happens to `quads` on conversion. A dynamic surface is
      triangles; state whether export re-derives none and says so
- [x] 1.4 DECIDE collapse placement for P0 — midpoint, projected midpoint, or
      the quadric the offline decimator already computes

## 2. The representation

- [x] 2.1 `include/clay/mesh/slot_pool.h` — stable slots, free list,
      generation per slot, no compaction on erase
- [x] 2.2 `include/clay/mesh/dynamic_surface.h` + `src/` — triangular
      half-edge over vertices, edges, half-edges and faces, all
      generation-tagged
- [x] 2.3 Attribute domains from the start: UV on the corner, colour and mask
      on the vertex. P0 need not author corner UVs; the representation SHALL
      be able to express them or the operators cannot be retrofitted
- [x] 2.4 Edge constraint flags: boundary, UV seam, sharp, material,
      user-locked
- [x] 2.5 `from_mesh` — validate indices, treat quads as provenance, weld by
      position, identify seams from the duplicates, carry colours and corner
      UVs, mark boundaries, build constraints
- [x] 2.6 `to_mesh` — contiguous arrays, export duplicates where corner
      attributes require a seam, clear `quads`, produce a mesh every existing
      consumer accepts unchanged
- [x] 2.7 `include/clay/mesh/dynamic_validate.h` — twin symmetry, closed next
      loops, three corners per face, no live reference to a dead slot, no face
      with a repeated vertex, consistent boundary links, no NaNs, consistent
      attribute domains
- [x] 2.8 Round-trip test: mesh → surface → mesh preserves geometry and the
      supported attributes under the stated seam semantics

## 3. The operators

- [ ] 3.1 `split_edge` — interior and boundary, interpolating position,
      colour, mask and corner UVs; normals recomputed locally rather than
      interpolated
- [ ] 3.2 `collapse_edge` — link-condition validity test, and REFUSAL on
      inversion, duplicate triangle, non-manifold result, boundary corruption,
      seam destruction, normal flip past a threshold, and zero-area output
- [ ] 3.3 `flip_edge` — refuses boundaries, constrained edges, an existing
      diagonal, orientation inversion, and any flip that does not improve the
      quality metric
- [ ] 3.4 Each operator is ATOMIC: it applies fully or leaves the surface
      exactly as it found it. A cancelled or refused operation SHALL NOT leave
      half an edge collapse behind
- [ ] 3.5 Every operator records into a `TopologyDelta` when given one
- [ ] 3.6 A validator run after every operator in the debug build
- [ ] 3.7 FUZZ: randomized valid patches under thousands of interleaved
      split/collapse/flip/move operations, validating every invariant. A
      mandatory CI target — rare local connectivity failures do not surface any
      other way

## 4. Spatial index

- [ ] 4.1 `include/clay/mesh/dynamic_bvh.h` — chunked leaves of a few hundred
      triangles with bounds and revisions, a top-level tree over the leaves
- [ ] 4.2 Insert, erase and update at leaf granularity; local leaf split and
      merge when a leaf grows or shrinks past its thresholds
- [ ] 4.3 Ball query, closest point and raycast, each checked against a
      brute-force oracle for exact parity
- [ ] 4.4 Dirty leaf tracking by epoch marks rather than a hash set per dab
- [ ] 4.5 A leaf-quality metric marks a rebuild; the rebuild runs between
      strokes, never mid-drag. The fixed BVH's own finding applies — a refit
      stays correct and does not stay fast, and a rebuild is not automatically
      an improvement
- [ ] 4.6 A topology mutation SHALL touch the leaves it changed and no others,
      asserted rather than profiled

## 5. The remesher

- [ ] 5.1 `include/clay/mesh/remesh_local.h` — target edge length with split
      and collapse factors, bounded passes and a bounded operation count per
      stamp
- [ ] 5.2 Hysteresis between the split and collapse thresholds, so a stationary
      brush cannot ping-pong an edge
- [ ] 5.3 Brush-relative detail as well as world and constant modes, so a
      smaller brush creates finer geometry without a second slider
- [ ] 5.4 Tangential relax after remeshing, constrained on boundaries and seams
- [ ] 5.5 Remesh timing per verb — before, after, or both — with the defaults
      recorded and justified per verb rather than shared
- [ ] 5.6 CONVERGENCE test: a stretched patch under repeated remeshing
      converges toward the target edge distribution without the surface
      drifting past a stated bound
- [ ] 5.7 Boundary and seam preservation tested directly, including a patch
      whose boundary would close under an unconstrained collapse

## 6. The sculptor

- [ ] 6.1 `mesh::DynamicSculptor` over the shared kernels from
      `add-shared-brush-kernels`. NO second copy of any deformation
- [ ] 6.2 Region gather by stable id through the chunked index; geodesic walk
      over the mutable adjacency with a bounded queue, preserving the path
      budget the fixed geodesic walk already uses
- [ ] 6.3 Mask gate support, identical in meaning to the fixed path
- [ ] 6.4 The verbs an adaptive surface offers, and the ones it does not,
      decided and documented rather than silently partial
- [ ] 6.5 Local normal recompute over the changed faces and their ring
- [ ] 6.6 PARITY: with topology changes disabled, a stamp on a dynamic surface
      and the same stamp on the same mesh through `MeshSculptor` agree within
      the stated tolerance. Anything larger means the kernels drifted
- [ ] 6.7 THE MILESTONE: a coarse sphere sculpted into a nose, an ear and a
      horn, with local geometry created and removed and no global remesh — as
      an example that renders and asserts

## 7. Undo and history

- [ ] 7.1 `mesh::TopologyDelta` — created and deleted vertices, edges and
      faces, connectivity changes, position and attribute changes, with
      `revert`, `apply`, `bytes`, and versioned encode/decode
- [ ] 7.2 COALESCED over a gesture: one entry per element, first `before` and
      last `after`, so the size follows the elements touched and not the stamps
      taken
- [ ] 7.3 Revert is BIT-EXACT and idempotent; revert-then-apply returns the
      surface exactly
- [ ] 7.4 `session::History` gains the step kind and a resolver, following the
      inversion the existing kinds use — `scene` may not see `mesh`, and the
      owner passes the resolver in
- [ ] 7.5 Journal encode, decode and replay for the new kind, with old
      journals still replaying
- [ ] 7.6 A decoder refuses hostile or truncated counts BEFORE allocating,
      matching `VertexDeltas::decode`'s defensive style
- [ ] 7.7 A compound step spanning a scene command and a topology delta undoes
      as one

## 8. Serialization

- [ ] 8.1 A versioned `DynamicSurface` encoding — magic, version, validated
      counts, overflow-checked sizes, declared attribute channels
- [ ] 8.2 Backward-open: an older reader skips the chunk and opens the document
      without it rather than failing
- [ ] 8.3 Round-trip bit-identical, including generations or an explicit
      statement that generations are not preserved and why

## 9. The C ABI and the host path

- [ ] 9.1 Opaque `clay_dynamic_surface` and `clay_dynamic_sculptor`; NO change
      to `clay_mesh_sculptor`'s semantics, which existing hosts rely on
- [ ] 9.2 Versioned descriptors for the surface, the topology policy and the
      stamp report, `struct_size` on input, bounded output fills
- [ ] 9.3 Triple revision — topology, geometry, attributes — so a host
      re-uploads an index buffer only when connectivity changed
- [ ] 9.4 Whole-surface export for correctness AND dirty-chunk transport for
      production, with the test comparing the two
- [ ] 9.5 Caller-owned buffers with a capacity query; no heap object per dirty
      chunk per frame
- [ ] 9.6 Borrowed position pointers are NOT offered where a mutation can
      invalidate them without a generation the caller can check
- [ ] 9.7 pyclay, and `tools/check_binding_parity.py` green
- [ ] 9.8 Swift smoke coverage on macOS and in the simulator
- [ ] 9.9 Version lines together — `CMakeLists.txt`, `bindings/c/clay.h`,
      `pyproject.toml`, `release_check.py`

## 10. Scale, memory and the gates

- [ ] 10.1 Benchmarks at 100k, 1M and 5M triangles with footprints of 500, 2k,
      10k and 50k vertices, timed per stage: candidate query, gather, split,
      collapse, flip, relax, deformation, normals, index update, dirty export
- [ ] 10.2 THE SCALING GATE: for a fixed footprint the stamp cost stays in one
      band as the surface grows 100k → 1M → 5M. A 50x model is not a 50x stamp
- [ ] 10.3 No full adjacency build and no whole-index rebuild on an ordinary
      dab, asserted by instrumentation rather than inferred from a timing
- [ ] 10.4 Memory per live vertex, edge, half-edge and face, per index
      triangle, and per undo stroke, reported rather than estimated
- [ ] 10.5 Cancellation on the long operations — construction, global remesh,
      conversion — through `parallel::CancelToken`, build-then-publish, and a
      cancelled operation leaves the surface byte-identical
- [ ] 10.6 Threading: topology mutation single-threaded and local; deformation,
      normals and leaf rebuilds parallel where disjoint. The pool runs a nested
      `parallel_for` inline, so one level per operation
- [ ] 10.7 Four presets green plus `release_check`; `tsan` under `setarch -R`
- [ ] 10.8 `python3 tools/check_layering.py` green
- [ ] 10.9 Docs: `docs/07-brushes-and-features.md` gains the third mesh mode
      and what it costs; `README.md`'s "deliberately does not do" entry is
      corrected rather than left contradicting the code; `docs/09` gains the
      measured latencies
