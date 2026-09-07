## 1. Classify what construction actually depends on

- [x] 1.1 MEASURED before designing: first create 120.8 ms, second 123.6 ms,
      `Adjacency::build` 120.2 ms, an `Adjacency` copy 0.20–0.78 ms, on 148,225
      vertices / 294,912 triangles
- [x] 1.2 Record the dependency of every constructor product: `Adjacency` is
      TOPOLOGY + THE POSITIONS IT WAS BUILT OVER (the weld epsilon partitions by
      position), `Bvh` is topology + geometry and is lazy, everything else is
      per-sculptor scratch
- [x] 1.3 Correct the roadmap's classification in the proposal rather than
      inheriting it

## 2. Share instead of copy

- [x] 2.1 `MeshSculptor` holds `std::shared_ptr<const Adjacency>`; a
      `const Adjacency&` member bound to it keeps every internal use unchanged
- [x] 2.2 Both existing constructors preserved, meaning exactly what they meant
- [x] 2.3 `MultiresSurface::level_adjacency_shared`; `MultiresSculptor::bind`
      shares the level adjacency instead of copying it
- [x] 2.4 `Adjacency::bytes()`

## 3. The cache

- [x] 3.1 `mesh::TopologyCache`, keyed by a caller-supplied stable identity —
      never by vertex/index counts
- [x] 3.2 `TopologyFingerprint` validates every hit: counts, weld epsilon and a
      hash of the index buffer. A mismatch is a miss
- [x] 3.3 MEASURE the fingerprint against what it decides to skip; it must be a
      rounding error on a build or it is not worth having
- [x] 3.4 `forget(owner)` on wholesale replacement — the intentional
      invalidation, with the fingerprint as the net under it
- [x] 3.5 Document-owned, never process-global: deterministic lifetime, released
      on close, no cross-document identity collision, isolated tests
- [x] 3.6 `release_unused()` releases only entries no live sculptor holds, which
      `shared_ptr` use counts make a fact rather than a heuristic
- [x] 3.7 Stats: entries, bytes, hits, misses, evictions, build and verify time

## 4. Host reachability

- [x] 4.1 `clay_document_topology_cache_stats` with a `struct_size` descriptor
- [x] 4.2 `clay_document_trim_topology_cache`, reporting bytes released
- [x] 4.3 `topology_cache` appended to `clay_memory_report`, inside `rebuildable`
- [x] 4.4 ABI 0.88.0 -> 0.89.0 in `CMakeLists.txt`, `clay.h`, `pyproject.toml`
- [x] 4.5 pyclay and Swift parity; `check_binding_parity.py` against a BUILT
      pyclay, not the fallback

## 5. Tests

- [x] 5.1 Two sculptors over one unchanged mesh share one topology object
- [x] 5.2 A position-only sculpt retains the entry
- [x] 5.3 A wholesale replacement invalidates it
- [x] 5.4 Identical vertex and index counts with different connectivity do NOT
      collide — proven with the revision deliberately left unmoved, which is the
      #472 shape
- [x] 5.5 Document close releases the cache; two documents do not collide
- [x] 5.6 Concurrent creation builds one authoritative entry and does not race
- [x] 5.7 Cached and uncached sculpt results are bit-identical
- [x] 5.8 A trim releases nothing an existing sculptor is holding
- [x] 5.9 PROVEN TO CATCH ITS REGRESSION: the fingerprint test fails when the
      fingerprint is removed

## 6. Verification

- [x] 6.1 Measured after the change and recorded in the proposal: first create
      121.3 ms, second 0.25 ms, after a position-only change 0.25 ms, after two
      indices swapped 121.3 ms, fingerprint 0.25 ms, 10,048,720 cache bytes. The
      GATE is the hit/miss count in the unit test, not any of these clocks
- [x] 6.2 Full unit suite green (2,537 cases, 9 ctest entries)
- [ ] 6.3 `python3 tools/release_check.py --skip-slow`
- [ ] 6.4 CI green

## 9. Porting it onto #488, after that merged

- [x] 9.1 The cache moved from the two binding handles into `io::ClaySpaceDoc`,
      beside the `mesh_geometry_revision` #488 put there. Both bindings take it
      from the document now
- [x] 9.2 The C ABI's own bump-and-forget helper is deleted -- it no longer
      exists to cite, which is why it is described here rather than named.
      `install_mesh_geometry` and `note_mesh_geometry_replaced` supersede it,
      and that was one of the two collisions agreed in the other session's
      favour
- [x] 9.3 `TopologyCache` declares its moves explicitly, with the moved-from
      cache left EMPTY and copying deleted. The `static_assert` beside
      `ClaySpaceDoc` FIRED on the real integration when only half the port was
      applied, naming the cause at the definition -- without it the error was
      "copy assignment is implicitly deleted" in `clayspace.cpp`, for a move
      nobody wrote
- [x] 9.4 FOUND AND FIXED -- the port forgot at `install_mesh_geometry` only,
      and claimed in its own comment that this was the one place triangles
      enter a layer. Main has TWO chokepoints: a weld rewrites triangles IN
      PLACE through `note_mesh_geometry_replaced` and never reaches the
      installer. Both forget now
- [x] 9.5 NOTHING WOULD HAVE FAILED, which is why it survived. A weld moves the
      vertex and triangle counts, so the fingerprint rejects the stale entry and
      rebuilds -- 120 ms where 0.25 ms was promised, invisible to every
      correctness test. The redundancy that makes the cache survivable is what
      made this fault undetectable; those are the same property
- [x] 9.6 The weld case proven by reverting the second forget: one assertion,
      the one that matters, on a clean build
