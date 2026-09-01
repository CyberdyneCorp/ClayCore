# Tasks: add-extreme-poly-runtime

- [ ] 0.1 SEQUENCING (see ROADMAP, "Phase 5 — the surface tier"): last of the
      five, because it optimises an architecture rather than compensating for a
      missing one — the reverted `add-item-spatial-index` is what that costs
      when it is done in the other order. FIRST to be pulled forward if an iPad
      build stalls on memory rather than on latency

## 1. Decide first

- [x] 1.1 DECIDE the chunk size from a benchmark matrix over 64, 128, 256, 512
      and 1024 triangles per leaf, measuring query cost, false-positive touched
      vertices, normal recompute, upload size, locality and topology mutation
      cost. Do not adopt a number from prior art without running it here.
      ANSWERED: 128 target / 32 min / 256 max, from
      `benchmarks/bench_surface_chunks.cpp` over a 2.08M-vertex plane at five
      footprints, three repeats at a stable load. design.md D2a records the
      table, the two harness defects that had reversed the ordering before the
      number was read, and the one place the decision departs from the letter
      of the rule: 64 and 128 tie on the term the rule minimises and the rule
      breaks a tie toward the smaller chunk ON THE GROUND that mutation cost
      grows superlinearly in chunk size — which the measurement falsifies.
      Mutation is a shallow U with its minimum at 128. The null hypothesis 256
      is beaten by 5-6% on the decision term, outside the spread, and 1.51x
      against 1.64x on false positives; it wins 14% on index memory, which the
      rule does not weigh and which is named as the cost
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
      NOT ticked, and now MEASURED rather than argued. It holds on the fixed
      mesh and the adaptive surface, where the tree is what `classes_in_ball`
      and `faces_in_ball` descend: 20M vertices against 1M at the same 20k
      footprint costs 1.05x the total dab. It is FALSE on the hierarchy, and
      the benchmark says by how much: 0.49 ms at 100k level vertices against
      1.58 ms at 1M for the same 1k footprint, about 1.2 ns a vertex, which is
      a scan. `MeshSculptor::surface_index` never builds a tree on its own
      behalf and `MultiresSculptor` is a caller that never picks, so nothing
      ever builds one for a level. Recorded in docs/09 with the numbers; the
      fix is 3.2 rather than a tree per level.
      STILL NOT TICKED, but the fix is now available rather than pending: 3.2
      landed, so a host that picks against a level can pass `seed_class` with
      `MultiresSculptor::seed_revision()` and the walk starts there instead of
      scanning. What keeps this open is that nothing supplies the seed
      AUTOMATICALLY — a caller that passes none still scans, so the requirement
      as written ("never a scan over every vertex") is not yet true of the
      hierarchy path on its own
- [x] 3.2 An optional caller-supplied seed from the pick subsystem, validated
      against a revision, so a stroke does not re-search a centre the host
      already picked
      DONE. `MeshBrushSettings::seed_revision` carries the token
      `MeshSculptor::seed_revision()` hands out, and a stamp falls back to the
      scan when it does not match; `MultiresSculptor::seed_revision()` binds and
      forwards, which is the hierarchy half. Zero means the caller claims
      nothing, so every shipped caller keeps the bounds check it had and this
      adds a way to be CORRECT rather than making anyone slower.
      Writing it found the defect that made it worth more than a query: a seed
      picked at one level and spent at another is IN BOUNDS, and
      `geodesic_region` returns an EMPTY region when the seed is farther than
      the radius from the centre — so a stale seed did not slightly misplace the
      dab, it silently lost it, indistinguishable from a fully masked stroke.
      Regression test in `test_multires_sculpt.cpp`, which spends the same stale
      seed three ways; proven to catch its regression (with the revision
      ignored, the stale case reports 0 moved). `stale_seeds_rejected()` is what
      makes the rejection assertable rather than assumed.
      NOW REACHABLE FROM EVERY HOST, which is what the defect deserved: the
      token rides on `clay_mesh_hit.seed_revision` and
      `clay_mesh_brush_desc.seed_revision`, with
      `clay_mesh_sculptor_seed_revision`,
      `clay_multires_sculptor_seed_revision` and
      `clay_mesh_sculptor_stale_seeds_rejected` beside them, and on pyclay's
      `raycast` dict, a `seed_revision` keyword on `stamp` / `apply_stroke`, and
      the two read-back properties. Zero and `None` claim nothing, so every
      caller written before the token is bit-identical. Cases in
      `test_c_mesh_sculpt.cpp`, `test_c_multires.cpp` and
      `bindings/python/tests/test_seed_and_peaks.py`, including the one that
      matters: an unrevisioned stale seed still LOSES the dab (0 moved) and the
      same seed WITH its token lands it
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
- [x] 3.7 A serial threshold below which a small footprint does not dispatch at
      all, measured rather than guessed
      MEASURED, by `benchmarks/bench_parallel_grain.cpp`, with the rule fixed
      before the data in D2's shape and recorded in design.md D2b. The answer is
      32,768 vertices (was 1024) and 576 chunks (was 4), stable across four
      sweeps at loads from 2.5 to 8.7.
      The finding is bigger than a tuned constant: the dispatch costs 17-20 us
      at ANY size, so at the shipped 1024 the parallel form was TEN TIMES SLOWER
      than the serial loop, and a chunk-parallel stamp at the old values would
      have been a pessimisation at every footprint this change benchmarks.
      Recorded honestly: NOTHING READS EITHER CONSTANT. The stamp path is serial
      and dispatches nothing, so these are the numbers a future parallel pass
      must start from rather than a description of what the library does — which
      is exactly the distinction ticking this on the constants' plausibility
      would have destroyed

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
      AND SAFE MID-STROKE, which is a separate claim from safe mid-save and was
      not true: see 7.6. The spec delta now states it and the cost is measured
      rather than assumed — `benchmarks/bench_trim_recovery` prices the dab
      after a warning at 0.62-2.04x for `Warning` (the sculpt level stays
      resident) and 13-182x for `Critical`, growing with the model, which is
      what makes "prefer Warning mid-drag, or hold a pin until the stroke ends"
      advice a host can act on rather than a preference
- [x] 4.5 A trim report saying what was released and how much
- [x] 4.6 THE GATE: after a critical trim, the authoritative checksum is
      unchanged and every dropped cache reconstructs to an identical surface
      DONE as `tests/unit/test_memory_trim.cpp`: the checksum and the cage
      position-by-position across a critical trim, every level's positions AND
      normals bit-identical after the rebuild, the chunk partition coming back
      naming the same faces under the same ids, every category the report names
      being a rebuildable one, the four pressures as a monotone chain with
      `None` releasing nothing, and the pin. Two defects found by writing it and
      fixed with the cases that catch them: a sculpted level's display normals
      were nobody's and moved by up to 0.02 across a rebuild, and
      `ScratchArena::trim` released nothing at all because `shrink_to_fit` is a
      no-op under `-fno-exceptions`
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
- [x] 6.5 The whole-surface path stays for correctness, and the test compares
      the reconstruction from the dirty stream against it
      DONE in all three places: `tests/unit/test_chunk_transport.cpp` for C++
      (hierarchy, adaptive surface and fixed mesh, including a SECOND edit
      drained on top of the first, which a reconstruction that rebuilt from the
      newest stream alone would fail), the C boundary in
      `test_c_surface_chunks.cpp` against `clay_multires_copy_level_mesh`, and
      pyclay in `test_surface_transport.py`. Compared as canonicalised
      TRIANGLES rather than as index buffers, because the two paths number
      their vertices differently on purpose, and canonicalised by ROTATION
      rather than by a sort, which would call an inside-out surface equal
- [x] 6.6 pyclay reaches the same transport, and parity is green
      DONE: `clay.SurfaceView` with numpy-native `copy_chunk`, plus the ledger,
      the trim, the profile, the preflights and a `clay.MemoryPin` context
      manager — `with` is the only form that cannot leave a document pinned
      when a stroke loop raises. `check_binding_parity.py` OK against the BUILT
      module (633 capabilities), and `bindings/python/tests/test_surface_transport.py`
      covers the same six cases the C test does

## 7. The gates

- [x] 7.1 A benchmark suite at 100k, 1M, 5M, 10M and 20M vertices with
      footprints of 1k, 5k, 20k, 100k and 500k, across fixed mesh, adaptive
      surface, multires, and multires with layers
      DONE as `benchmarks/bench_extreme_poly.cpp` plus
      `tools/bench_extreme_poly.py`. RUN: the fixed mesh at all five sizes and
      all five footprints (a footprint larger than the model is SKIPPED and
      says so rather than measuring the boundary — 500k needs 5M vertices to
      be interior); the adaptive surface and the hierarchy at 100k and 1M. The
      20M rows were run and are in docs/09. The LAYERED rows are scripted and
      recorded as awaiting the rebase: this branch is cut from main and does
      not carry add-mesh-sculpt-layers, and a layer stack of the benchmark's
      own invention would be a measurement of the benchmark
- [ ] 7.2 Per-stage timing — seed, chunk query, gather, geodesic, snapshot,
      weight, alpha, automask, kernel, remesh, detail write, normals, index
      update, readback — because a total without stages is not actionable
      PARTIAL, and the benchmark says which half. Six stages are timed as
      themselves because each is its own call: seed, chunk query, index update,
      remesh, detail write and readback. The eight inside `MeshSculptor::stamp`
      — gather, geodesic, snapshot, weight, alpha, automask, kernel, normals —
      are ONE bucket named `stamp*`, because the library has no per-stage
      timers, adding them would perturb what is being measured, and it would
      touch a file two concurrent branches are also editing. Named in the
      output rather than folded silently into a total. The stage split was
      already actionable: it is what located the hierarchy's seed scan in 3.1
- [x] 7.3 THE LOCALITY GATE: for one footprint, stamp time stays in one band
      from 1M to 20M vertices
      DONE twice. As a ctest gate at sizes CI can afford,
      `tests/unit/test_extreme_poly_scaling.cpp` at 9,409 against 148,225
      vertices on both query shapes (a geodesic verb and a ball verb): the
      workset, the write region and the peak workset are IDENTICAL and the
      median time ratio is 0.94 to 1.9 against a model ratio of 16, banded at
      4.0 because the box is shared. Proven to catch its regression — with
      `surface_index` returning null the ratio goes to 6.4 and 4.4 and the gate
      fails, while the allocation gate still passes, which is the division of
      labour design.md describes. And as the benchmark at the sizes the
      requirement names: 20M against 1M at the same 20k footprint is 1.05x the
      total dab, 1.01x for the stamp itself
- [x] 7.4 THE ALLOCATION GATE: after warm-up an ordinary stable-topology stamp
      performs no heap allocation; adaptive stamps allocate only from
      preallocated pools
      DONE with all three of D7's assertions, in the two places the mechanisms
      live. Count and bytes, at TWO model sizes sixteen times apart with the
      same world footprint, in `test_sculpt_allocation.cpp` — zero and zero at
      both, with the workset and write region identical, so the gate cannot be
      satisfied by a runtime whose cost follows the model. The high-water
      assertion in `test_scratch_arena.cpp`, on `memory::ScratchArena`, which is
      where the mechanism is: the sculptor's gather does not consume the arena
      yet (3.1, 3.7), and asserting it there would assert something nothing
      implements
- [x] 7.5 THE PREVIEW GATE: bytes handed to a host per stamp follow the dirty
      chunks and not the model size
      DONE in `test_chunk_transport.cpp` (a hierarchy at 10 against 40 base
      quads a side: 16x the model, 1.36x the bytes, with the FULL upload
      asserted to have grown so that "the dirty bytes did not" is a claim),
      in `test_extreme_poly_scaling.cpp` on the fixed mesh, and in pyclay. The
      example measures it too: 9.2 KiB on both of two models 16x apart
- [x] 7.6 THE MEMORY-PRESSURE GATE: 4.6, as a test rather than a claim
      DONE: `tests/unit/test_memory_trim.cpp`, and the C and pyclay halves in
      `test_c_surface_chunks.cpp` and `test_surface_transport.py`. Proven to
      catch its regression: with `trim_blocked` returning false the pin cases
      fail
      EXTENDED, and writing the extension found the defect the gate as first
      written could not see. Every case above trims a document nobody is
      touching, which is the half a host can schedule. A trim arrives from an
      operating-system callback and lands BETWEEN TWO DABS OF A DRAG, and there
      the gate was false: `MultiresSculptor::bind` decides whether the `Mesh&`
      its `MeshSculptor` holds is still live by comparing
      `cache_generation()`, which was bumped when a level cache was BUILT and
      not when it was released — so between a `drop_*_caches` and the next
      build the number had not moved and the stale sculptor was kept, bound to
      a freed `LevelCache`. It did not crash: the stamp wrote into released
      storage, `absorb_level_edit` rebuilt the level from the authoritative
      detail before reading the displacement back, and the dab was not there,
      with `stamp` still returning the weld-class count it believed it had
      moved. With a trim after every dab, every SECOND dab vanished.
      Fixed by moving the generation on release too, in the three `drop_*`
      functions. Four cases hold it — `test_extreme_poly_exactness.cpp` (the
      named regression and the determinism form of it), `test_c_surface_chunks.cpp`
      and `test_surface_transport.py` — and it is PROVEN to catch its
      regression twice over: with the release bump removed the branch still
      COMPILES, the C++ cases fail on the checksum standing still, and the same
      case under `asan-ubsan` reports `heap-use-after-free` in
      `MeshSculptor::valid()` reading storage freed by
      `MultiresSurface::drop_all_caches`
- [x] 7.7 Peak telemetry — scratch, workset, dirty chunks, topology operations
      — reported as high-water marks for profile tuning
      DONE. The type and its high-water semantics were already here; what this
      stage added is the ENGINE reporting into it, which is what the task
      actually asks for. Four quantities, four owners, each publishing through a
      borrowed `PeakTelemetry*` the host sets and the engine only writes:
      `ScratchArena::end_stamp` (before `used_` is cleared and there is nothing
      left to report), `MeshSculptor::gather` (once the workset is FINAL, after
      the falloff and automask drop what they drop), `ChunkTable::enter_dirty`
      (the one place the set grows, so a chunk marked twice in an epoch counts
      once) and `DynamicSculptor::stamp` (topology operations plus the adaptive
      workset, from a thin wrapper so a body with several early returns has one
      publish site). Null is the default and costs a null check.
      `test_extreme_poly_scaling.cpp` now READS the peak the engine accumulated
      instead of calling `observe_workset` itself, which is the difference the
      task names, and the wiring has its own cases there and in
      `test_dynamic_sculpt.cpp` — including that a fresh partition is entirely
      dirty (the largest the set ever is, and what a staging buffer must hold)
      and that a later smaller stamp does not pull a peak down
- [ ] 7.8 Measured on the reference iPad as well as on a desktop: median and
      p95 stamp latency, footprint and peak memory, and a sustained multi-minute
      session for thermal behaviour. The device gate already exists and this
      extends it rather than inventing a second one
      NOT DONE and NOT DOABLE HERE: there is no reference iPad on this box, and
      `release_check.py`'s `device` row refuses for the same reason. The desktop
      half is done and is in docs/09
- [ ] 7.9 Four presets green plus `release_check`; `tsan` under `setarch -R`;
      `check_layering.py` green including whatever 1.2 decided
      PARTIAL, and the report says exactly which. `cpu-only` green (2079 cases).
      `asan-ubsan` green over the new cases. `tsan` under `setarch -R` run over
      the new cases. `check_layering.py`, `check_c_abi.py`,
      `check_binding_parity.py`, `check_gallery.py` and `check_swift_package.py`
      all green. `release_check.py` still reports the four pre-existing failures
      this environment has (bindings, abi, tests and wheel from the anaconda
      GLIBCXX_3.4.31 mismatch, device from the hardware gate). The metal, cuda,
      opencl and vulkan presets need toolkits this box does not have
- [x] 7.10 Docs: `docs/09-brush-latency-and-coverage.md` gains the new
      representations' measured costs; the memory documentation gains the
      eviction order verbatim, because a host implementer needs it in prose
      DONE: docs/09 gains "The extreme-poly runtime, measured" — the
      20M-against-1M per-stage ratio table, the absolutes at 1M for scale, the
      per-dab upload, the chunk-size decision, the adaptive and hierarchy rows,
      and the measured gap 3.1 names. docs/05 already carried the eviction order
      verbatim from the engine stage and now carries the tests that prove each
      sentence of it — including the one that was not true.
      EXTENDED: docs/09 gains "What a memory warning costs the dab after it" —
      the two-run ratio table from `bench_trim_recovery` and the defect it was
      written to price — and docs/05 gains the paragraph a host needs beside the
      eviction order: a trim mid-drag is correct at every pressure and is free
      only at `Warning`
