# Tasks: expose-the-brick-cache

## 1. The two missing primitives

- [x] 1.1 `clay_eval_grid` + `clay_grid_query`: dense lattice evaluation with an
      optional cull region (both bounds or neither, finite, non-empty). The ABI
      had no way to evaluate a grid at all, which is what filling a brick is.
- [x] 1.2 `clay_layer_node_influence_bound` / `clay_layer_influence_bound`:
      three states through `out_has_bounds` + `out_infinite`, because an item
      with a non-local op or an unbounded primitive has no finite box and
      claiming one would let a caller cull it away

## 2. The cache surface

- [x] 2.1 Opaque handle from a versioned `clay_brick_config`, never bound to a
      document — matching the C++ class, which is document-free
- [x] 2.2 `mark_dirty` / `mark_dirty_nodes` / `mark_dirty_layer`, `take_dirty`,
      `eval_requests`, `submit`, `brick_bounds`, `cull_region`, `sample`,
      `read_bricks`, `surface_bricks`, `stats`, `build_mip`, `current_lod`,
      `mesh`, `raycast`
- [x] 2.3 `clay_brick_request` is byte-for-byte `brick::BrickRequest`, pinned by
      `offsetof`/`sizeof` static assertions, so a drain is a copy
- [x] 2.4 One engine change only: `BrickCache::tracked_count()`
- [x] 2.5 Batched everywhere it matters — `eval_requests` and `submit` take
      arrays, `read_bricks` reads many at a fixed stride
- [x] 2.6 `take_dirty` is capacity-in / count-out with a remainder, not a NULL
      size query: a size query would have to drain to answer

## 3. The span guard

- [x] 3.1 `BrickCache::mark_dirty` casts a float region to int brick
      coordinates with no range check and then allocates per brick in the span.
      Validated in 64-bit at the boundary: non-finite, empty, coordinate outside
      int32, span above the batch ceiling — each refused with the cache
      unchanged.
- [x] 3.2 "Dirty everything" is the absence of a region, not an infinity

## 4. Found in review, fixed

- [x] 4.1 `eval_requests` culled each request against the brick's BARE world
      box, with a doc comment arguing that an item whose influence bound misses
      the box cannot change a sample in it. That is false for a BANDED cache: a
      sample stores its true distance whenever that distance is within the band,
      so an item outside the box but within a band of it decides samples inside.
      `cache.h` says so in as many words — `cull_region()` returns the brick
      dilated by the band and tells you to hand THAT to the compiler.
      Demonstrated: a sphere 0.13 from a brick's nearest sample, band 0.15, was
      dropped; the sample read `CLAY_TAPE_FAR` and the brick was classified
      Outside instead of Surface.
      Fixed first with an explicit `band` parameter on `eval_requests` — and
      review was right to reject that: a bare float nothing can cross-check
      makes `0`, the value a zeroed variable or a forgetful wrapper supplies,
      silently restore the very defect. The band rides on the REQUEST now, as
      origin/spacing/dims already do and for the same stated reason, so there
      is no wrong value to pass. `brick::BrickRequest` grew the field too, so
      the byte-for-byte layout still holds and is still asserted.
      Regression test asserts both the sample value and the classification, and
      fails on the un-dilated version with `3.4e+37` against `0.13`.

## 4b. Found by the adversarial review, fixed

- [x] 4.2 SEVERITY 5, and a pre-existing ENGINE bug this change made reachable.
      `BrickCache::mark_dirty`'s infinite branch cleared the queue and re-added
      through `dirty_one`, which pushes only a brick that is not already
      queued — and the queued FLAGS survived the clear. Every brick already
      waiting was dropped and could never be re-queued by any later call: the
      cache served its stale samples for the rest of its life, and only being
      destroyed recovered. Reproduced at the engine level: after
      `mark_dirty(infinite)` the dirty count is 0 where 8 bricks were waiting.
      Reached by the documented way of saying "this edit's influence is
      unbounded", which is what a plane or an intersect op produces.
      Fixed in `src/brick/cache.cpp`; regression test in `test_brick.cpp`
      covers both the un-drained and the fully-drained starting state.
- [x] 4.3 The span guard borrowed `CLAY_MAX_BATCH`, which everywhere else in
      this ABI counts values in a TRANSIENT batch freed at the end of the call.
      Here each unit is a PERMANENT map entry of about a hundred bytes, so the
      same number authorised 1.6 GB of bookkeeping from six floats, and
      `memory_budget` covers stored payloads rather than this. Given its own
      ceiling now, checked against the cache's CURRENT size as well, since the
      guard is per call and a session marks many regions. Measured: the region
      that cost 1.6 GB is refused with RSS unchanged; a 2-unit region still
      tracks 343 bricks and is accepted.
- [x] 4.4 The `take_dirty` doc claimed refusing a NULL size query meant "a first
      fill of a large region cannot force the host into one unbounded malloc".
      It only relocated the allocation: the engine's drain is all-or-nothing, so
      asking for one request after a million-brick mark still stages the
      million inside the library. The header now says so, and says the fix needs
      a capacity-aware drain in the engine.

## 5. Tests

- [x] 5.1 `tests/unit/test_c_brick.cpp`, 13 cases: config and every refusal;
      influence bounds incl. the infinite cases; `clay_eval_grid` culled equals
      unculled bit for bit; the span guard leaving the tracked count unchanged;
      the whole host loop (mark, chunked drain, eval, submit) with sampled
      bricks agreeing with `clay_eval_points` band-clamped; `read_bricks` states
      and uniform fill; generation/staleness; the memory budget; mesh and
      raycast; and the band regression above.
- [x] 5.2 The incremental claim is proven by a COUNT of re-evaluated bricks, not
      a timing

## Known and not fixed here

Recorded rather than quietly carried. None is reachable without first marking a
region far larger than a sculpt does, and each wants a decision this change is
not the place for:

- The staging cliff above is documented, not removed. Removing it needs
  `BrickCache::take_dirty` to take a capacity so it pops at most that many keys
  and bumps only their generations — an engine API change.
- `build_mip` allocates fp16 lattices that `memory_budget` does not cover and
  `clay_brick_stats.memory_usage` does not report.
- `mesh` and `raycast` can overflow int32 on brick keys the cache will itself
  accept at the extremes of the coordinate range.
- A finite region built from ±FLT_MAX reads as infinite and so means "dirty
  everything tracked" rather than being refused as absurd.

## Deliberately not done

- pyclay coverage. Printed as an outstanding follow-up by
  `check_binding_parity.py` rather than filed as an exemption, because the gate
  runs one way and would not otherwise have said anything.
- `dim` is accepted as 8 or 16 only, which is what `BrickConfig` documents but
  not what it enforces. If another dim is ever wanted, the C++ contract moves
  first.
