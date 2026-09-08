## 1. Measure before designing

- [x] 1.1 Instrument both accessors — `cross_level_of` in
      `src/mesh/multires_eval.cpp` and `MultiresSurface::cross_level_at` — with a
      read and a refresh count, and record what an INTERIOR dab costs. 10 dabs on
      the issue's 4x4-at-level-3 fixture: 48 reads, 48 refreshes, 0 full
      rebuilds, 0 partial updates, 0 vertices evaluated. The walk ran 4.8 times
      per dab while the hierarchy evaluated nothing
- [x] 1.2 Reproduce the issue's fixture EXACTLY rather than approximately, so the
      numbers are comparable: a 16x16 cage, a 4x4 block refined to level 3, giving
      1,089 stored vertices, 136 outside and 132 derived faces; and an 8x8 block
      to level 4, giving 16,641, 520 and 516

## 2. Enumerate every writer of a level's positions

- [x] 2.1 Enumerate them, including the three that do not pass through
      `multires_eval.cpp` at all: `MultiresSculptor::bind`, `stamp_coarse` and
      the `fine_targets_` write-back all go through
      `MultiresSurface::level_mesh`, which is the only door onto a mutable
      `Mesh&` and has exactly those three callers outside the tests. The table is
      in `design.md`
- [x] 2.2 PROVE the enumerable-sites-only counter is wrong rather than asserting
      it: implement it, and record which gates it passes. It passes the interior
      dab gate, passes the level-2 stroke gate, and serves a stale rim after a
      stroke on the CAGE — 0 of 13 outside positions moved
- [x] 2.3 REJECT bumping at `MultiresSurface::level_mesh` on the numbers, not on
      taste: `stamp_coarse` calls it for every coarse level on every dab, so the
      bound level would refresh every dab and the win would be zero

## 3. The signal

- [x] 3.1 `MultiresLevel::positions_revision` in `src/mesh/multires_internal.h`,
      on the LEVEL rather than in `LevelCache`, so a cache drop and a
      bit-identical rebuild do not read as "nothing changed"
- [x] 3.2 `pending` and `pending_all` become private behind
      `MultiresLevel::note_moved`, `note_moved_all` and `clear_pending`, and the
      first two move the revision. Route all thirteen mutation sites in
      `src/mesh/multires_eval.cpp`, `src/mesh/multires.cpp`,
      `src/mesh/project.cpp` and `src/mesh/sculpt_layer_eval.cpp` through them
- [x] 3.3 `clear_pending` does NOT move the revision: being consumed by the level
      above is not a change
- [x] 3.4 `restore_positions` does NOT move it either, and the reason is written
      beside it — it rewrites what the stored coefficients reconstruct to, which
      is what the current revision already names, and the raw brush write it
      undoes never moved the number

## 4. The gate on the read side

- [x] 4.1 `LevelCache::cross_parent_revision` beside `cross`, so
      `MultiresSurface::release_cross_levels` and every `drop_*_caches` take both
- [x] 4.2 `cross_level_of` and `MultiresSurface::cross_level_at` refresh only on a
      mismatch, and write the revision back on the same line
- [x] 4.3 `MultiresEvalStats::cross_level_reads` and
      `MultiresEvalStats::cross_level_refreshes` in
      `include/clay/mesh/multires.h`, reported by pyclay's `eval_stats`. No C
      mirror exists, so no ABI surface moves

## 5. Gates, each proved by reverting one thing

- [x] 5.1 "regional: an interior dab does not walk the region rim again" in
      `tests/unit/test_multires_regional.cpp` — reads >= 10, refreshes == 0, and
      the outside positions still equal a cold build. Restoring the unconditional
      refresh compiles and fails it: CHECK( 48 == 0 )
- [x] 5.2 "regional: the rim is walked again when the level below moves" —
      refreshes >= 1, 29 of 136 outside positions moved, values equal to a cold
      build. Never refreshing compiles and fails it: CHECK( 0 >= 1 ),
      CHECK( 0 == 29 ), and fails the pre-existing gate with CHECK( 0 == 23 )
- [x] 5.3 "regional: a stroke on the CAGE moves the level above's outside
      positions" — 13 of 72 outside positions moved, values equal to a cold
      build. The enumerable-sites-only counter compiles, passes 5.1 and 5.2, and
      fails this: CHECK( 0 == 13 )
- [x] 5.4 The pre-existing gate "regional: the outside positions follow a stroke
      on the level below" passes UNCHANGED

## 6. Measure again

- [x] 6.1 Two builds differing in one condition, min of 200 interior dabs each,
      alternated three times with the load average sampled before and after every
      run (0.74 throughout). 4x4 at level 3: 1,319 rim walks -> 0, 0.02962 ->
      0.02440 ms per dab, ratio 0.82. 8x8 at level 4: 1,759 -> 0, 0.14273 ->
      0.10763 ms, ratio 0.75
- [x] 6.2 Both builds moved the same vertices — 5,960 and 27,579 over the 200
      dabs — so the difference is the rim walk and nothing else
