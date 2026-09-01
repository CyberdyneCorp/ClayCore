# Tasks: add-extreme-poly-runtime

- [ ] 0.1 SEQUENCING (see ROADMAP, "Phase 5 — the surface tier"): last of the
      five, because it optimises an architecture rather than compensating for a
      missing one — the reverted `add-item-spatial-index` is what that costs
      when it is done in the other order. FIRST to be pulled forward if an iPad
      build stalls on memory rather than on latency

## 1. Decide first

- [ ] 1.1 DECIDE the chunk size from a benchmark matrix over 64, 128, 256, 512
      and 1024 triangles per leaf, measuring query cost, false-positive touched
      vertices, normal recompute, upload size, locality and topology mutation
      cost. Do not adopt a number from prior art without running it here.
      NOT ticked by the design stage: design.md D2 fixes the matrix, the six
      measured quantities and the decision rule for reading them, and leaves
      the NUMBER to the benchmark. Ticking this on an unrun benchmark is the
      failure the task was written against. Null hypothesis to beat: 256/64/512,
      `DynamicBvhOptions`'s current and unmeasured defaults
- [x] 1.2 DECIDE where the memory profile lives — a new `memory` module with a
      layering entry, or `io` beside the existing report. `parallel` is the
      precedent for a new leaf module; `io::MemoryReport` is the precedent for
      the type
- [x] 1.3 DECIDE whether the interactive budget is a hint or a contract, per
      deferrable item. Deferring exact normals during a drag is safe; deferring
      a topology decision changes the committed result and is not
- [x] 1.4 DECIDE how much residency policy the engine owns. A host that must
      ask before every level switch is a bad API; an engine that evicts on its
      own is a document mutating behind a host that may be mid-save

## 2. The shared chunk

- [x] 2.1 `include/clay/mesh/surface_chunks.h` — bounds, triangles, unique
      vertices, and separate revisions for topology, geometry, normals and
      attributes
- [x] 2.2 ONE unit serving the spatial index leaf, the brush candidate set, the
      parallel work unit, the normal recompute unit, the dirty set and the host
      upload unit. A subsystem that invents a second granularity SHALL say why
      DONE: `ChunkTable` is the one unit; `SurfaceLeaf` is now a name for
      `SurfaceChunk`. The multires partitioner states in its own file why a
      (base patch, quadrant at depth d) chunk is the same granularity and not a
      second one
- [x] 2.3 An epoch-marked dirty set rather than a hash set per stamp
- [x] 2.4 Chunk-local vertex indexing for derived buffers, with the mapping to
      global identity kept; authoritative topology stays 32-bit
- [x] 2.5 Integrate with the adaptive surface and with each multires level
      DONE for the adaptive surface (`DynamicBvh` publishes into the table)
      and for each multires level (`ensure_level_chunks`, in the level CACHE so a
      trim releases it). The FIXED sculptor gets `partition_mesh_chunks` and does
      not yet mark chunks beside its weld classes — that is design.md's open
      question, blocked on the 1.1 measurement

## 3. Local update path
- [ ] 3.1 The query path is: brush volume → top-level tree → candidate chunks →
      candidate vertices → exact footprint. Never a scan over every vertex
- [ ] 3.2 An optional caller-supplied seed from the pick subsystem, validated
      against a revision, so a stroke does not re-search a centre the host
      already picked
- [x] 3.3 Local normals over the write region and its ring, with an optional
      deferral to stroke end whose FINAL state is exact
      ALREADY TRUE in `MeshSculptor` (`defer_normals` / `flush_normals` over
      the write region and its ring); what this stage added is the profile field
      that expresses the deferral and the `NormalFlush` maintenance item, which is
      marked non-optional because the committed state has to be exact
- [x] 3.4 Spatial index quality tracked; a rebuild is marked and deferred, and
      it runs between strokes rather than mid-drag
- [x] 3.5 A deferred-maintenance queue — index quality rebuild, cache
      compaction, sparse-to-dense conversion, slot-pool compaction — that a
      host services with a time budget between interactions and that never runs
      in a pointer event
- [x] 3.6 Parallel granularity chosen per operation, one level only: the pool
      runs a nested `parallel_for` inline by design, so parallel-chunks inside
      parallel-vertices is a mistake the code must not make
      DONE as a RULE the code keeps: `kChunkParallelGrain` and
      `kVertexParallelGrain` are one level each, documented against the pool's
      inline-nesting behaviour. No new nested dispatch was introduced
- [ ] 3.7 A serial threshold below which a small footprint does not dispatch at
      all, measured rather than guessed

## 4. Memory

- [x] 4.1 `SculptMemoryProfile` with a memory class and byte budgets, filled by
      the HOST. NO device detection, no platform API, no `if iPad` in the
      portable core — the policy has to be testable on a desktop
- [x] 4.2 Extend the memory report with the new categories: adaptive surface
      content, multires authoritative detail, sculpt layers, sculpt undo; and
      separately the rebuildable ones — chunk indices, per-level runtime
      caches, evaluated layer caches, scratch, preview staging
- [x] 4.3 Roll up three totals a host can act on: essential, rebuildable,
      undoable
- [x] 4.4 `trim(pressure)` with the eviction order written into the spec:
      transient scratch, preview buffers, evaluated caches, inactive spatial
      indices, inactive derived positions, other rebuildable caches, history to
      the host's policy — and NEVER unsaved authoritative content
      DONE: `mesh::trim_surface` for the hierarchy and the adaptive surface,
      `ScratchArena::trim` for the scratch, in the order the spec fixes, with
      `memory::MemoryPin` making a trim honest during a save. History is left to
      the host's own policy and is never touched here
- [x] 4.5 A trim report saying what was released and how much
- [ ] 4.6 THE GATE: after a critical trim, the authoritative checksum is
      unchanged and every dropped cache reconstructs to an identical surface
- [x] 4.7 Residency: sculpt level and display level resident by default on a
      constrained profile, other levels compact detail only
      DONE via `MultiresSurface::set_memory_profile`, applied at the residency
      changes the HOST causes (the two level setters) and nowhere else — an
      engine evicting on its own high-water mark is the second invalidation
      source design.md D5 refuses
- [x] 4.8 Scratch capacity tracks the largest recent footprint with a soft and
      a hard bound; past the hard bound the work is processed in blocks rather
      than allocated

## 5. Preflight

- [x] 5.1 A capacity estimate — authoritative, runtime and PEAK bytes — for
      any operation whose peak exceeds its result: adding a level, converting
      between representations, flattening a stack, global remesh, serialization
- [x] 5.2 Refuse with a typed budget error BEFORE allocating, never after
      allocating half. The peak, not the steady state, is what kills an app on
      the target device
      DONE for `add_level`, which is the one operation that already carries a
      budget; the other four report a typed refusal to a caller that passes one
      to their preflight, and allocate nothing to do it
- [x] 5.3 Checked arithmetic on every estimate; an overflow reports a refusal
      rather than a small number
- [x] 5.4 Build-then-publish on every such operation, and cancellation through
      the existing token
      ALREADY TRUE for the five named operations (`from_mesh`, `to_mesh`,
      `add_level`, `project_from`, `encode` are all build-then-publish and take
      the existing cancel token); verified rather than re-implemented

## 6. Transport

- [x] 6.1 Revisioned dirty-chunk C ABI: count, info, positions, normals,
      indices, and an acknowledgement so a host can drain incrementally
      DONE as `clay_surface_view`, ONE seam over all three representations
      rather than a third set of entry points beside the shipped
      `clay_dynamic_surface_chunk_*` and `clay_multires_copy_block`, which stay
      byte-compatible. `clay_chunk_info` is a bulk fill and is registered in
      `check_c_abi.py`'s ARRAY_ELEMENT_STRUCTS with its reason
- [x] 6.2 Caller-owned buffers with a capacity query; no heap object per chunk
      per frame
      DONE: every buffer null IS the capacity query, and a buffer too small
      writes NOTHING rather than a partial fill a host might draw
- [x] 6.3 Topology revision distinct from geometry revision, so an index buffer
      is re-uploaded only when connectivity changed
      DONE: `clay_chunk_revisions` carries all four, and the single `revision`
      a shipped host reads stays beside them as the maximum
- [x] 6.4 A stale-revision result is discardable by the host: the revision it
      requested is echoed with what the engine is at now
- [ ] 6.5 The whole-surface path stays for correctness, and the test compares
      the reconstruction from the dirty stream against it
      NOT DONE, and deliberately left to the test stage: the whole-surface
      paths are untouched and `tests/unit/test_c_surface_chunks.cpp` asserts
      that the chunks COVER the surface exactly once (triangle count against
      the source mesh, and against `clay_multires_level_counts`), but a full
      reconstruction compared against the whole-surface path is
      `test_chunk_transport.cpp`, which is not written
- [x] 6.6 pyclay reaches the same transport, and parity is green
      DONE: `clay.SurfaceView` with numpy-native `copy_chunk`, plus the ledger,
      the trim, the profile, the preflights and a `clay.MemoryPin` context
      manager — `with` is the only form that cannot leave a document pinned
      when a stroke loop raises. `check_binding_parity.py` OK against the BUILT
      module (633 capabilities), and `bindings/python/tests/test_surface_transport.py`
      covers the same six cases the C test does

## 7. The gates

- [ ] 7.1 A benchmark suite at 100k, 1M, 5M, 10M and 20M vertices with
      footprints of 1k, 5k, 20k, 100k and 500k, across fixed mesh, adaptive
      surface, multires, and multires with layers
- [ ] 7.2 Per-stage timing — seed, chunk query, gather, geodesic, snapshot,
      weight, alpha, automask, kernel, remesh, detail write, normals, index
      update, readback — because a total without stages is not actionable
- [ ] 7.3 THE LOCALITY GATE: for one footprint, stamp time stays in one band
      from 1M to 20M vertices
- [ ] 7.4 THE ALLOCATION GATE: after warm-up an ordinary stable-topology stamp
      performs no heap allocation; adaptive stamps allocate only from
      preallocated pools
- [ ] 7.5 THE PREVIEW GATE: bytes handed to a host per stamp follow the dirty
      chunks and not the model size
- [ ] 7.6 THE MEMORY-PRESSURE GATE: 4.6, as a test rather than a claim
- [ ] 7.7 Peak telemetry — scratch, workset, dirty chunks, topology operations
      — reported as high-water marks for profile tuning
- [ ] 7.8 Measured on the reference iPad as well as on a desktop: median and
      p95 stamp latency, footprint and peak memory, and a sustained multi-minute
      session for thermal behaviour. The device gate already exists and this
      extends it rather than inventing a second one
- [ ] 7.9 Four presets green plus `release_check`; `tsan` under `setarch -R`;
      `check_layering.py` green including whatever 1.2 decided
- [ ] 7.10 Docs: `docs/09-brush-latency-and-coverage.md` gains the new
      representations' measured costs; the memory documentation gains the
      eviction order verbatim, because a host implementer needs it in prose
