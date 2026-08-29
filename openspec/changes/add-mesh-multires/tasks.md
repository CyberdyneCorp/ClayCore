# Tasks: add-mesh-multires

- [ ] 0.1 SEQUENCING (see ROADMAP, "Phase 5 — the surface tier"): after
      `add-shared-brush-kernels` and `add-dynamic-topology`, whose chunk
      runtime it shares. `add-mesh-sculpt-layers` follows it and its detail
      storage is designed HERE — building the two independently is how a
      library ends up with two displacement systems that disagree

## 1. Decide first

- [ ] 1.1 DECIDE the first subdivision rule — Catmull-Clark for the imported
      quad character topology this library already exports and reimports, or
      Loop for arbitrary triangles including whatever an adaptive surface
      produced. The hierarchy abstraction SHALL take either; the first
      implementation decides which workflow lands first
- [ ] 1.2 DECIDE the detail storage unit — blocked sparse, with the block size
      and the sparse-to-dense promotion threshold chosen from a measurement
      rather than from a round number
- [ ] 1.3 DECIDE whether a level may exist over a REGION only, as
      `add_level_region` allows on the voxel side, or whether a mesh level is
      whole. Read that implementation first: outside a refined region the voxel
      level has no storage and reads its parent, which is what makes the
      transition watertight by construction
- [ ] 1.4 DECIDE whether per-level index buffers are cached or re-derived, and
      state the memory consequence either way

## 2. Frames

- [ ] 2.1 `include/clay/mesh/surface_frame.h` — build and transport an
      orthonormal frame from the subdivided parent
- [ ] 2.2 UV tangent where a valid parametrization exists; a deterministic
      geometric tangent otherwise; a pinned fallback at degenerate valences
- [ ] 2.3 Transport by shortest-arc rotation when the parent normal moves,
      with sign consistency enforced against the previous frame
- [ ] 2.4 Test: orthonormal, deterministic, no sign flip under a small
      deformation, and a normal-only wrinkle stays normal to a bent surface

## 3. Detail

- [ ] 3.1 `include/clay/mesh/detail_field.h` — tangent/bitangent/normal
      coefficients, blocked sparse storage with a present mask, dense
      promotion, byte accounting, coverage query, compaction
- [ ] 3.2 fp32 authoritative. NO quantization of the editing representation in
      this change: high-frequency detail is exactly where a visible artefact
      appears first, and compression waits for a measured error bound
- [ ] 3.3 Demotion and compaction never run inside a pointer event
- [ ] 3.4 Versioned encode/decode, with the decoder refusing hostile counts
      before allocating
- [ ] 3.5 Test: sparse set/get, zero clears an entry, promotion, byte
      accounting, deterministic round trip

## 4. The hierarchy

- [ ] 4.1 `include/clay/mesh/subdivide.h` — the rule chosen in 1.1, with
      parent/child stencils, boundary rules, and face-varying interpolation
      that does NOT average a UV across a seam
- [ ] 4.2 `include/clay/mesh/multires.h` — base mesh, hierarchy, per-level
      detail, `add_level`, `remove_highest_level`, level count
- [ ] 4.3 `P(n) = Subdivide(P(n-1)) + Detail(n)` as the evaluation, with
      `mesh_at_level` exporting an ordinary `mesh::Mesh` at any level
- [ ] 4.4 Deterministic level generation: the same base and rule produce the
      same hierarchy on every platform
- [ ] 4.5 Adding a level PREFLIGHTS — positions, detail, normals, indices and
      any spatial index it would build — and refuses with a typed budget error
      rather than allocating half of it
- [ ] 4.6 Build-then-publish on add and remove, so a failure or a cancellation
      leaves the surface untouched
- [ ] 4.7 A hierarchy carrying detail REFUSES an arbitrary base topology
      change, and the refusal names the conversion that is the supported route

## 5. Sculpting

- [ ] 5.1 `mesh::MultiresSculptor` over the shared kernels. NO third copy of
      any deformation
- [ ] 5.2 Sculpt level and display level are independent and separately set
- [ ] 5.3 A write at the active level converts a displacement into detail
      coefficients in the transported frame
- [ ] 5.4 Local propagation: dirty parent vertices → the child stencils
      depending on them → dirty child vertices, per level, and nothing else
- [ ] 5.5 Local normals: the active level's changed region, then the
      propagated regions above, never a whole level
- [ ] 5.6 Per-level runtime cache — `Adjacency` and `Bvh` built lazily, keyed
      on the level's revision, and droppable without touching detail
- [ ] 5.7 Masks and the stroke engine reach a multires surface exactly as they
      reach the other representations

## 6. Projection

- [ ] 6.1 `include/clay/mesh/project.h` — `project_surface`, normal-ray first
      with a closest-point fallback and a maximum distance
- [ ] 6.2 `transfer_attributes` semantics UNCHANGED: it still moves no
      position, and the two functions share the query and not the contract
- [ ] 6.3 Initialize a hierarchy from a sculpted source: subdivide the new base,
      project onto the source, store the difference as detail
- [ ] 6.4 Test: a dyntopo sculpt projected onto a clean base reproduces the
      source within a stated tolerance

## 7. Undo, history, serialization

- [ ] 7.1 `mesh::MultiresDelta` — level, entries, before and after detail or
      base values, coalesced per gesture
- [ ] 7.2 Records EDITED state only. Derived higher-level positions are a
      cache and SHALL NOT multiply an undo step by the level count
- [ ] 7.3 `session::History` gains the kind and a resolver, through the same
      inversion the other kinds use
- [ ] 7.4 Journal encode, decode, replay; older journals still replay
- [ ] 7.5 A versioned `MultiresSurface` encoding — base, hierarchy metadata
      including the subdivision rule, per-level detail blocks — backward-open,
      with counts validated before allocation
- [ ] 7.6 Round trip bit-identical, including which level was active

## 8. Bindings and gates

- [ ] 8.1 C ABI: opaque handles, level operations, sculpt and display level,
      export at a level, changed-block readback, revisions for base, detail and
      evaluated state
- [ ] 8.2 `struct_size` on every descriptor, bounded output fills, caller-owned
      readback buffers
- [ ] 8.3 pyclay, and `tools/check_binding_parity.py` green
- [ ] 8.4 Swift smoke on macOS and in the simulator
- [ ] 8.5 THE MILESTONE, as a numbered example that renders and asserts:
      sculpt detail at a fine level, change proportions at a coarse one, return
      and find the detail present and attached
- [ ] 8.6 Version lines together — `CMakeLists.txt`, `bindings/c/clay.h`,
      `pyproject.toml`, `release_check.py`
- [ ] 8.7 Four presets green plus `release_check`; `tsan` under `setarch -R`
- [ ] 8.8 `python3 tools/check_layering.py` green

## 9. Scale and memory

- [ ] 9.1 Benchmark cold whole-hierarchy reconstruction against local
      propagated reconstruction, reporting affected vertices per level
- [ ] 9.2 THE GATE: a low-level edit touching a small fraction of the base does
      not reconstruct a whole higher level
- [ ] 9.3 Memory per level: detail bytes, topology-map bytes, runtime adjacency
      and index bytes, derived cache bytes, reported rather than estimated
- [ ] 9.4 Inactive-level runtime caches are droppable, and dropping them leaves
      the authoritative detail checksum unchanged
- [ ] 9.5 Cancellation on add-level, projection, flatten and serialization
- [ ] 9.6 Docs: `docs/07-brushes-and-features.md` gains the hierarchy and what
      each level costs; the ROADMAP's reversed non-goal is cited rather than
      restated; `docs/09` gains the measured latencies
