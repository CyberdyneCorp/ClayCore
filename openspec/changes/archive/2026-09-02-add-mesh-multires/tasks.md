# Tasks: add-mesh-multires

- [x] 0.1 SEQUENCING (see ROADMAP, "Phase 5 — the surface tier"): after
      `add-shared-brush-kernels` and `add-dynamic-topology`, whose chunk
      runtime it shares. `add-mesh-sculpt-layers` follows it and its detail
      storage is designed HERE — building the two independently is how a
      library ends up with two displacement systems that disagree

## 1. Decide first

- [x] 1.1 **CATMULL-CLARK.** `mesh::Mesh` already carries quads beside the
      triangles, an imported retopologised character is quads, and the rule
      turns a triangle cage into quads at level 1 — so every level above 0 is
      pure quads and everything that reads a level has one face arity to
      handle. Loop would have served whatever an adaptive surface produced;
      that is a second rule the abstraction takes (`SubdivisionRule`, recorded
      in the encoding) and not a second implementation this change owed
- [x] 1.2 **BLOCKED SPARSE, fp32, 1024 vertices a block, promotion at 1.0.**
      A block exists only once something in it is non-zero and is released by
      `compact` when it returns to zero. BOTH NUMBERS ARE MEASURED, and the
      block size is a PARAMETER rather than a constant so that they can be:
      `BM_MultiresDetailBlockSize` sweeps 64 / 256 / 1024 / 4096 across three
      footprints on a level of 1,048,576 vertices and reports what each costs
      to hold the same detail (blocks allocated plus the block table over the
      level) —

          reached      64      256     1024     4096      total KiB
              400   72.25       25       16       49
            4,000  139.75       94       88       97
           40,000  814.75      769      760      769

      1024 is the least at every footprint: below it the table dominates, above
      it the last partly-used block does. The minimum is broad rather than
      sharp, which is why this is a default rather than a tuning knob.

      DENSE PROMOTION AT 1.0, and the measurement is why the first guess (0.75)
      was wrong. The reason to promote was supposed to be the per-read
      indirection, and `BM_MultiresDetailAccess` says there is none worth
      having: 611 M/s sparse against 605 M/s dense over a stroke-sized index
      set, because the block-table entry a local footprint keeps re-reading is
      always in cache. Promotion buys no measurable speed, while promoting
      EARLY costs real memory — a field promoted at three-quarters coverage
      allocates a third more than the sparse form it replaced. A threshold with
      nothing on the benefit side belongs where it cannot cost anything either
- [x] 1.3 **A LEVEL IS WHOLE.** No region-scoped levels in this change, and the
      reason is that the voxel construction does not transfer: outside a
      refined region a voxel level has no storage and reads its parent, which
      works because the lattice is regular and "the parent cell" is arithmetic.
      On an irregular surface the transition has to be OWNED by particular
      faces, and a half-refined quad ring is a topology question rather than a
      storage one. The artist request is the same and the problem is not, so it
      is deferred rather than approximated
- [x] 1.4 **PER-LEVEL FACE LISTS ARE CACHED; everything else is derived.**
      Authoritative per level is the quad list plus a base-patch id per face —
      16 bytes a face, so 4^L * 16 * F0, which is 82 MB for level 4 over a 20k
      cage and is exactly why `preflight_add_level` exists. Edges, the
      corner->edge map, the vertex incidences, the level's own `mesh::Mesh`,
      its `Adjacency` and its evaluated arrays are rebuilt on demand into a
      droppable per-level cache. Re-deriving the face lists too is possible —
      they follow from the cage and the rule — and was rejected because every
      level below the one in use would then be rebuilt to evaluate it

## 2. Frames

- [x] 2.1 `include/clay/mesh/surface_frame.h` — build and transport an
      orthonormal frame from the subdivided parent
- [x] 2.2 UV tangent where a valid parametrization exists; a deterministic
      geometric tangent otherwise; a pinned fallback at degenerate valences
- [x] 2.3 Transport by shortest-arc rotation when the parent normal moves,
      with sign consistency enforced against the previous frame
- [x] 2.4 Test (`tests/unit/test_surface_frame.cpp`): orthonormal, deterministic, no sign flip under a small
      deformation, and a normal-only wrinkle stays normal to a bent surface

## 3. Detail

- [x] 3.1 `include/clay/mesh/detail_field.h` — tangent/bitangent/normal
      coefficients, blocked sparse storage with a present mask, dense
      promotion, byte accounting, coverage query, compaction
- [x] 3.2 fp32 authoritative. NO quantization of the editing representation in
      this change: high-frequency detail is exactly where a visible artefact
      appears first, and compression waits for a measured error bound
- [x] 3.3 Demotion and compaction never run inside a pointer event
- [x] 3.4 Versioned encode/decode, with the decoder refusing hostile counts
      before allocating
- [x] 3.5 Test (`tests/unit/test_detail_field.cpp`): sparse set/get, zero clears an entry, promotion, byte
      accounting, deterministic round trip

## 4. The hierarchy

- [x] 4.1 `include/clay/mesh/subdivide.h` — the rule chosen in 1.1, with
      parent/child stencils, boundary rules, and face-varying interpolation
      that does NOT average a UV across a seam
- [x] 4.2 `include/clay/mesh/multires.h` — base mesh, hierarchy, per-level
      detail, `add_level`, `remove_highest_level`, level count
- [x] 4.3 `P(n) = Subdivide(P(n-1)) + Detail(n)` as the evaluation, with
      `mesh_at_level` exporting an ordinary `mesh::Mesh` at any level
- [x] 4.4 Deterministic level generation: the same base and rule produce the
      same hierarchy on every platform
- [x] 4.5 Adding a level PREFLIGHTS — positions, detail, normals, indices and
      any spatial index it would build — and refuses with a typed budget error
      rather than allocating half of it
- [x] 4.6 Build-then-publish on add and remove, so a failure or a cancellation
      leaves the surface untouched
- [x] 4.7 A hierarchy carrying detail REFUSES an arbitrary base topology
      change, and the refusal names the conversion that is the supported route

## 5. Sculpting

- [x] 5.1 `mesh::MultiresSculptor` over the shared kernels. NO third copy of
      any deformation
- [x] 5.2 Sculpt level and display level are independent and separately set
- [x] 5.3 A write at the active level converts a displacement into detail
      coefficients in the transported frame
- [x] 5.4 Local propagation: dirty parent vertices → the child stencils
      depending on them → dirty child vertices, per level, and nothing else
- [x] 5.5 Local normals: the active level's changed region, then the
      propagated regions above, never a whole level
- [x] 5.6 Per-level runtime cache — `Adjacency` and `Bvh` built lazily, keyed
      on the level's revision, and droppable without touching detail
- [x] 5.7 Masks AND the stroke engine reach a multires surface exactly as they
      reach the other representations. The mask is a `field::MaskGate` on
      `MultiresSculptor::stamp` and the composed weight order is unchanged,
      because it is the fixed sculptor's. The stroke is
      `brush::apply_to_multires`, and it shares the per-stamp resolution with
      `apply_to_mesh` rather than copying it — `mesh_stamp_settings`,
      `mesh_mask_gate` and `mesh_automask_inputs` are one implementation each,
      so where a stamp lands, how far it reaches and how hard it presses cannot
      drift between the two representations. Reachable from C
      (`clay_multires_sculptor_apply_stroke`) and from pyclay
      (`MultiresSculptor.apply_stroke`). An adaptive surface still has no
      stroke entry point, which is now the only mesh representation without
      one

## 6. Projection

- [x] 6.1 `include/clay/mesh/project.h` — `project_surface`, normal-ray first
      with a closest-point fallback and a maximum distance
- [x] 6.2 `transfer_attributes` semantics UNCHANGED: it still moves no
      position, and the two functions share the query and not the contract
- [x] 6.3 Initialize a hierarchy from a sculpted source: subdivide the new base,
      project onto the source, store the difference as detail
- [x] 6.4 Test (`tests/unit/test_mesh_project.cpp`): a sculpted reference
      projected onto a clean cage reproduces the source within a stated
      tolerance (0.03 on a 0.35-amplitude field), the normal ray answers more
      often than the closest-point fallback, and a vertex with no
      correspondence inside the distance limit is left exactly where it was

## 7. Undo, history, serialization

- [x] 7.1 `mesh::MultiresDelta` — level, entries, before and after detail or
      base values, coalesced per gesture
- [x] 7.2 Records EDITED state only. Derived higher-level positions are a
      cache and SHALL NOT multiply an undo step by the level count
- [x] 7.3 `session::History` gains the kind and a resolver, through the same
      inversion the other kinds use
- [x] 7.4 Journal encode, decode, replay; older journals still replay
- [x] 7.5 A versioned `MultiresSurface` encoding — base, hierarchy metadata
      including the subdivision rule, per-level detail blocks — backward-open,
      with counts validated before allocation
- [x] 7.6 Round trip bit-identical, including which level was active

## 8. Bindings and gates

- [x] 8.1 C ABI: opaque handles, level operations, sculpt and display level,
      export at a level, changed-block readback, revisions for base, detail and
      evaluated state
- [x] 8.2 `struct_size` on every descriptor, bounded output fills, caller-owned
      readback buffers
- [x] 8.3 pyclay, and `tools/check_binding_parity.py` green
- [x] 8.4 Swift smoke on macOS and in the simulator
- [x] 8.5 THE MILESTONE (`examples/68_mesh_multires.py`), as a numbered example that renders and asserts:
      sculpt detail at a fine level, change proportions at a coarse one, return
      and find the detail present and attached
- [x] 8.6 Version lines together — 0.74.0 across — `CMakeLists.txt`, `bindings/c/clay.h`,
      `pyproject.toml`, `release_check.py`
- [x] 8.7 `cpu-only`, `asan-ubsan` and `tsan` (under `setarch -R`) green;
      `release_check` green apart from the four this machine cannot run — see
      the PR body, which names each and the environment cause
- [x] 8.8 `python3 tools/check_layering.py` green — no new edge; `mesh` already reaches everything the hierarchy needs, and `session` already reaches `mesh`

## 9. Scale and memory

- [x] 9.1 Benchmark cold whole-hierarchy reconstruction against local
      propagated reconstruction, reporting affected vertices per level
- [x] 9.2 THE GATE: a low-level edit touching a small fraction of the base does
      not reconstruct a whole higher level
- [x] 9.3 Memory per level: detail bytes, topology-map bytes, runtime adjacency
      and index bytes, derived cache bytes, reported rather than estimated
- [x] 9.4 Inactive-level runtime caches are droppable, and dropping them leaves
      the authoritative detail checksum unchanged
- [x] 9.5 Cancellation on add-level, projection, flatten and serialization
- [x] 9.6 Docs: `docs/07-brushes-and-features.md` section 8d gains the
      hierarchy and what each level costs; the ROADMAP's reversed non-goal is
      cited rather than restated and its Phase 5 row records what shipped;
      `docs/09` gains the measured latencies and the ratio the gate holds
