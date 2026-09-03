# Tasks: add-sdf-prefix-cache

## 1. Establish what is already there

- [x] 1.1 `add-sdf-sculpt-transaction` §17.4 is the open item this closes:
      "LOCAL CHECKPOINTS for Smooth, so `begin()` stops being O(model).
      Deliberately after the benchmark, not before it." The benchmark it
      waited on — `BM_SdfSmoothTransactionBegin` — exists
- [x] 1.2 `seed-a-suffix-tape` already names the fold boundary:
      `scene::compile_layer_prefix`, `scene::compile_layer_suffix` and
      `eval::eval_points_seeded`. Neither the emitter nor the seeded evaluator
      is rebuilt here
- [x] 1.3 `reuse-the-tape-prefix` already established that a stable prefix of
      an edit list produces byte-identical output, and that appending is the
      dominant sculpt pattern
- [x] 1.4 Re-measured the cost this exists to remove (#306), one dab into 12
      bricks at a 0.05 voxel: **0.23 ms at 200 items, 1.86 ms at 5,000,
      18.07 ms at 50,000.** Consolidating the layer shows the floor — the same
      work falls to about a third — and costs the artist their history
- [x] 1.5 `field::relax_in_place` and `FieldVolume::rewrite_region_tallied`
      landed with the transaction and are unchanged in arithmetic here; the
      rewrite gains only an optional record of WHICH bricks moved
- [x] 1.6 `scene::consolidate_layer` is `bake_layer` then
      `replace_layer_with_volume`; `bake_layer` is still one function that
      needs a `Layer`, which a caller holding a bare tape does not have

## 2. `bake_tape` — the half of a bake that needs no layer

- [x] 2.1 `scene::bake_tape(tape, params, want_color, out_cost, point_eval,
      token)`; `bake_layer` becomes "compile a local view" plus this, so there
      is ONE definition of what a baked volume is
- [x] 2.2 The caller owes the two things a tape cannot state for itself: THE
      FRAME (the layer visible, its own transform identity — sampling the
      world-space field and putting the result back under the layer applies the
      transform twice), and `want_color`, because the compiler folds colour
      into instructions and a tape can no longer be asked
- [x] 2.3 `layer_colors_vary` stays public for exactly that reason: it is the
      rule any caller of `bake_tape` has to apply to get the same bytes
- [x] 2.4 `tape` is BORROWED and must outlive the call

## 3. Lazy Smooth: a working field materialized around the brush

- [x] 3.1 `FieldVolume::empty_lattice(region, cell_size, band)` — the index,
      the brick counts and the far bounds, and an empty sample store. Every
      brick reads as sample-free until something fills it
- [x] 3.2 `SdfSmoothTransaction::begin` EVALUATES NOTHING: a compile of the
      layer, an index for the working lattice, and a digest. The whole-layer
      bake that used to be here is gone
- [x] 3.3 `dependency_region` is derived from relax's own stencil rather than
      guessed: the rewrite ball, plus relax's silent widening of a too-narrow
      falloff, plus `radius_cells`, plus **a brick DIAGONAL** — `rewrite_region`
      writes whole bricks that merely TOUCH the ball, so a written sample can
      sit a diagonal out. `sqrt(3)` rounded up to 1.75
- [x] 3.4 The working set follows the BRUSH and not the model: with the brush
      region fixed, adding geometry far away does not change what a dab
      materializes. A count, so it holds on a loaded machine
- [x] 3.5 A region radius of zero means EVERYWHERE — a filter, not a brush — and
      its dependency is the whole lattice. The eager cost, correctly
- [x] 3.6 A dab materializes what it will READ and then relaxes; a brick
      already materialized is counted and LEFT ALONE, because refilling it
      would throw away the edits earlier dabs made to it
- [x] 3.7 A cancel while filling returns before the dab runs: what was
      materialized is source values and is sound, and a dab must not run on a
      half-filled region
- [x] 3.8 `SdfSmoothMaterializationStats { materialized_bricks, reused_bricks,
      updates }` — counts, which is the form a scaling claim can be tested in
- [x] 3.9 `commit` assembles the layer ONCE through the same source and on the
      same lattice, materializes the edited region (a dab can move the surface
      into a brick the source called empty), overlays exactly what the dabs
      changed, then redistances and compacts once
- [x] 3.10 A gesture that changed NOTHING installs nothing: no volume, no undo
      entry, no consolidation. A pointer-down and pointer-up with no effective
      dab must not be a way to lose an artist's parametric history
- [x] 3.11 THE ONE SEMANTIC DIFFERENCE, stated rather than hidden: the old path
      relaxed a REDISTANCED bake and this redistances a RELAXED field. Exact
      parity is not reachable for a local field, because the old start point was
      globally post-processed

## 4. `materialize_region` — sparsity taken as given

- [x] 4.1 FORCE every brick meeting the region to store samples from `fill`,
      whether or not the values look near the surface. APPENDS to the store
- [x] 4.2 Why not `resample_region`: it genuinely creates bricks from
      `kBrickEmpty`, but it re-DECIDES sparsity and rebuilds the whole store to
      do it — O(stored) plus `shrink_to_fit` plus a full-lattice chamfer, which
      is a bake's cost per dab
- [x] 4.3 Why not a `kBrickUnknown` sentinel: `kBrickEmpty` already means "no
      surface here, and here is which side", a reading consumers rely on. A
      third state hidden in a two-state sentinel needs changes at ~9 call sites
      plus the blob validator
- [x] 4.4 The far bounds are NOT re-derived — that array is a two-pass chamfer
      over the lattice, and a caller materializing a region is by definition
      about to read inside it, where the stored samples answer
- [x] 4.5 `ResampleTally` reused, so `added` / `kept` are the counters a
      scaling test needs
- [x] 4.6 `brick_stored_at(p)` for the lazily-filled caller's question

## 5. Naming which bricks went stale

- [x] 5.1 `FieldVolume::BrickCoord { x, y, z }` — a bounding box says where to
      look; this says what to fetch
- [x] 5.2 `brick_origin(coord)` — the world position of a brick's first sample
- [x] 5.3 `read_brick(coord, out)` — one brick's stored samples, x-fastest over
      `(kBrickDim+1)^3`, the order `sample_blocks` fills and `to_blob` writes.
      False when that brick stores none, and then `out` is untouched
- [x] 5.4 `rewrite_region_tallied` gains an optional `out_changed`, APPENDED
      with the coordinate of every brick in which a stored sample actually
      moved — not every brick selected, which is what `touched_bricks` counts
- [x] 5.5 The vector is the CALLER'S, so a gesture reuses one across dabs
      rather than allocating per dab; duplicates across calls are the caller's
      to fold
- [x] 5.6 `materialize_region` gains the same `out_added`, because a
      materialized brick is new bytes to a consumer exactly as a rewritten one
      is

## 6. The prefix field cache

- [x] 6.1 `SdfPrefixPolicy`: `cell_size` (required, > 0), `band`, `padding`
      with `ConsolidationParams` meanings, plus `min_history_roots`,
      `keep_live_suffix_roots` and `max_bytes`
- [x] 6.2 `max_bytes == 0` DISABLES the cache — not "unbounded". A cache with no
      ceiling is a leak on a device with a memory budget, and "off" is the safe
      reading of a field nobody filled in. A zeroed policy caches nothing
- [x] 6.3 `prefix_boundary_for(layer, policy)` — top-level roots only, never
      inside a group (a group is one root and its children are not a boundary
      the fold reaches). Returns a root COUNT, which is what keeps that a P0
      restriction rather than a shape to grow out of
- [x] 6.4 An empty prefix is not a prefix, and a boundary at the very end
      leaves no suffix for a seed to fold onto — `compile_layer_suffix` refuses
      that, so decline rather than build a volume nothing can use
- [x] 6.5 `SdfPrefixCache::find` re-checks the digest on EVERY hit and drops a
      stale entry; `build` compiles the prefix and bakes it; `invalidate_layer`
      and `clear` are the optimisation half
- [x] 6.6 The key is layer + boundary + RESOLUTION: the same prefix sampled at
      two cell sizes is two caches and neither is wrong
- [x] 6.7 THE PREFIX IS BAKED WITHOUT REDISTANCE. Redistance replaces every
      sample with the distance the samples imply, which is right for a volume
      that is about to BE the layer and wrong for one reproducing an
      ACCUMULATOR. **Measured with it on: 0.063 — two cells — where the raw
      samples differ by float rounding.** Compact rides along, because it only
      runs after a successful redistance and dropping bricks would shrink the
      region the far-bound rule calls covered
- [x] 6.8 THE LATTICE IS THE WHOLE LAYER'S, NOT THE PREFIX'S. `sample_blocks`
      takes its origin straight from `region.min`, so a different region is a
      different lattice. **Measured over the prefix's own padded bounds:
      0.0074. Over the layer's region: 3.3e-7.** It is also right on its own
      terms — the prefix must answer wherever the SUFFIX might need it, and a
      suffix grows the surface where the prefix never reached
- [x] 6.9 The colour question is asked of the NODES the prefix covers, not of
      the tape and not of the whole layer: a prefix that is all one colour must
      not grow a channel because a suffix item is red
- [x] 6.10 LRU eviction to `max_bytes`, and lowering the ceiling evicts at once.
      A budget too small to hold one prefix returns null rather than a pointer
      into a freed entry
- [x] 6.11 A cancelled build caches NOTHING
- [x] 6.12 `SdfPrefixCacheStats`: entries, bytes, hits, misses, builds,
      evictions, invalidations, `seeded_windows`, `fallback_windows`

## 7. The digest, in one place

- [x] 7.1 `src/session/layer_digest.h` — PRIVATE to `src/session`, not
      installed, no ABI. What is public is the two questions it answers
- [x] 7.2 `mix_layer_head` (everything about a layer that is not a root) and
      `mix_roots(layer, count)` (the first `count` roots and nothing after
      them), shared so a whole-layer digest and a prefix digest cannot disagree
      about what a layer IS — a prefix built under one mirror and reused under
      another is a different field
- [x] 7.3 `layer_prefix_fingerprint(layer, count)` is separate from
      `layer_fingerprint` because an APPEND would otherwise throw away a cache
      that is still perfectly valid — which is the entire reason it exists
- [x] 7.4 The count is MIXED IN as well as bounding the walk: a prefix of three
      roots and one of four that agree on the first three are different
      digests, because a boundary is part of what is being identified
- [x] 7.5 The CONTENT, not the pointer — an instance layer shares its edit
      list, so an edit through a sibling is an edit here and the shared address
      has not moved
- [x] 7.6 `mix()` refuses aggregates by `static_assert`: a struct mixed
      wholesale folds its PADDING in, and padding is not a value
- [x] 7.7 THE DIGEST IS THE SAFETY NET, not the optimisation. Command-aware
      invalidation can be forgotten; this cannot, because it is computed from
      what the layer holds right now. A missed invalidation is wrong geometry;
      a redundant one is only slow

## 8. `SdfSourceField` — the layer's field, as cheaply as it can be answered correctly

- [x] 8.1 `open(doc, layer, cache, policy, point_eval, token)`; a null cache is
      simply the full walk, which is what makes every accelerated path optional
      rather than load-bearing
- [x] 8.2 NEVER BUILDS. This is the call a Smooth transaction makes at
      pointer-down, and a bake there would be the whole-layer cost the lazy
      path exists to remove. `SdfPrefixCache::build` is the door for a host with
      somewhere to put that work
- [x] 8.3 The frame is the LAYER'S OWN, exactly as `bake_layer` samples it — the
      layer visible and its transform identity — so a volume built through this
      composes with, and can be installed under, the layer the way a
      consolidation's can. Hence its own shallow view of the document; `Layer`
      holds `SdfContent` by `shared_ptr`, so node ids are the caller's and the
      suffix can name them
- [x] 8.4 THE FAR-BOUND RULE. The volume seeds a window only where it stores
      EVERY sample of it; anywhere else the prefix TAPE is evaluated for that
      window. **Measured: 3e-7 where the volume stores samples, 0.27 — about 14
      CELLS — where it does not**, because `eval` outside stored bricks is a
      conservative far bound and not the historical distance. Categorical:
      barely moves with cell size or blend width
- [x] 8.5 Decided per WINDOW, not per point, so the fast path is a straight loop
      and the slow one is a whole extra tape evaluation rather than a branch in
      the inner loop
- [x] 8.6 ...and per BRICK inside `block_fill`, not per 512-brick run: one
      uncovered brick must not drag a whole window onto the slow path
- [x] 8.7 The prefix tape is compiled ONCE at open, not per fallback window: a
      window that falls back is already paying the walk and should not pay a
      compile as well
- [x] 8.8 Every failure below `open` is a fall-back to `full_`, never an error:
      a refused suffix compile, a missing entry, a declined boundary
- [x] 8.9 `accelerated()`, `prefix_roots()`, `suffix_roots()`, and
      `note_seeded()` feeding the composition counters
- [x] 8.10 What "correct" means, all three regimes stated because a source that
      claimed to be "the field" would be lying at the second: **on the lattice
      it was built for, 3.3e-7; between lattice points, 7.6e-3 at a 0.03 cell —
      a quarter of a cell, ordinary trilinear error and NOT the walk's answer;
      outside the prefix's stored bricks, exact, because the far-bound rule
      sent it to the tape**
- [x] 8.11 `SdfSculptPolicy` gains `prefix`, and the gesture's three sampling
      numbers are COPIED over it, so a caller cannot ask for a prefix at a
      resolution the gesture is not using — a seed off a different lattice is an
      interpolation rather than the stored sample

## 9. Tests: the prefix cache (`tests/unit/test_sdf_prefix_cache.cpp`)

- [x] 9.1 THE PARITY MATRIX, and it is both the first and the last thing the
      file does: an accelerated source equals the full walk over random points,
      across boundaries, blend widths and ops. The cache is only ever allowed to
      be faster
- [x] 9.2 The far-bound measurement itself, reproduced as a test rather than
      quoted: seeded where the volume stores samples against seeded where it
      does not, so the 3e-7 / 14-cell split is a thing that fails if the rule is
      dropped. Plus exactness ON THE LATTICE the prefix was built for
- [x] 9.3 APPENDING after the boundary keeps the prefix — the case a whole-layer
      digest would throw away, and the reason the prefix digest exists
- [x] 9.4 Editing the SUFFIX keeps the prefix; editing the PREFIX does not
- [x] 9.5 A stale entry is never served, whatever moved: a root, a parameter, a
      deformer, a layer property — and an edit through a SIBLING INSTANCE
      sharing the same `SdfContent`, which is the case a pointer digest gets
      wrong
- [x] 9.6 Opening a source never BUILDS one: `builds` is unchanged and the
      source is unaccelerated
- [x] 9.7 With no cache at all, and with a disabled policy, the source is still
      exactly the full walk
- [x] 9.8 A cancelled build caches nothing
- [x] 9.9 THE MEMORY BUDGET: eviction is least-recently-used, lowering the
      ceiling evicts at once, and zero means OFF rather than unbounded
- [x] 9.10 `invalidate_layer` drops that layer's entries and only that layer's
- [x] 9.11 The fingerprint pair cannot drift:
      `layer_prefix_fingerprint(l, roots.size()) == layer_fingerprint(l)`

## 10. Tests: the lazy Smooth (`tests/unit/test_sdf_sculpt.cpp`)

- [x] 10.1 THE DEFINING TEST: `begin` materializes nothing and stores no
      bricks; the first dab brings in only what it will read; the SAME dab
      again brings in nothing new and reuses what is there; a dab elsewhere
      materializes its own ball and not the model
- [x] 10.2 Distant unrelated model does not change what a dab materializes —
      the same count with 0 and with 400 far-away items. A count, not a clock
- [x] 10.3 The lazy working field against a whole-layer relax of the same
      lattice, SPLIT by whether the sample is in the band, because past the band
      both hold a bound rather than a distance and hold different ones on
      purpose. **In band: 5.5e-5, about a thousandth of a cell**, from
      force-stored bricks at the band edge changing relax's renormalization
- [x] 10.4 How far the lazy commit sits from the whole-layer path it replaces,
      measured on the surface: **0.0037, or 0.073 of a cell.** Recorded as a
      bound rather than a target — what matters is that it is a fraction of a
      cell and not a feature
- [x] 10.5 The FOUR byte-identity cases from `add-sdf-sculpt-transaction` were
      rewritten to the contract that is true of a local field. They asserted a
      globally post-processed start point that a lazy path does not have; they
      now assert parity where parity holds and measure the rest
- [x] 10.6 A gesture that changed nothing commits nothing: no volume, no undo
      entry, and the layer's parametric items are still there
- [x] 10.7 Everything the transaction already promised still holds unchanged —
      the mid-gesture serialization, the one undo step, the stale refusal, the
      policy cases

## 11. Tests: the incremental preview delta (`tests/unit/test_c_sdf_sculpt.cpp`)

- [x] 11.1 A short buffer takes NOTHING, reports what it needs, and leaves the
      delta waiting — then the grown buffer takes all of it. A partial drain
      would strand bricks nothing reports a second time, because taking is what
      clears them
- [x] 11.2 THE WHOLE POINT: the same tiny dab hands over the same number of
      bricks on a layer with 300 more items in it. Payload follows the brush,
      not the model
- [x] 11.3 The delta ACCUMULATES across frames and dedups by brick: the same dab
      twice reports the same bricks once, and the generation is 2 because the
      preview did move twice
- [x] 11.4 Nothing waiting means no generation and no bounds; taking clears the
      delta and NOT the generation, because the generation names the state the
      caller now holds
- [x] 11.5 A spent transaction refuses both calls rather than dangling: after
      commit or cancel the working field is released and a delta read would
      describe something the host can no longer draw
- [x] 11.6 `tests/c_api/smoke.c` drives the whole thing from pure C11 — begin,
      dab, ask, size, take — so the header is proved to be C, not C++ that
      compiles
- [x] 11.7 `tools/check_c_abi.py` learns that `clay_sdf_preview_brick` is an
      ARRAY ELEMENT and therefore carries no `struct_size`, exactly as
      `clay_brick_request` and `clay_voxel_chunk_mesh_range` do

## 12. Tests: `bake_tape` (`tests/unit/test_consolidate.cpp`)

- [x] 12.1 `bake_tape` reproduces `bake_layer` BYTE FOR BYTE, with the caller's
      half of the contract spelled out in the test: the layer's own frame,
      visible, and the colour question asked of the NODES
- [x] 12.2 Colour on exactly the same rule, both ways round: passing
      `want_color` wrongly is different BYTES, not a slower path
- [x] 12.3 `bake_tape` refuses what `bake_layer` refuses — no cell size, an
      empty tape — and a cancelled token discards rather than returning a
      partial bake

## 13. Build and layering

- [x] 13.1 `src/session/sdf_prefix_cache.cpp` into `CMakeLists.txt`
- [x] 13.2 `tests/unit/test_sdf_prefix_cache.cpp` into `tests/CMakeLists.txt`
- [x] 13.3 `tools/check_layering.py` gains `session -> eval`, documented in the
      table beside the entry: `eval`'s own set is already a subset of
      `session`'s and nothing in `eval` includes `session`, so no transitive
      edge and no cycle; `mesh`, `pick` and `io` already depend on `eval` at
      this height. The `BakePointEval` injection pattern is for the opposite
      case — a module BELOW `eval` that must not name it
- [x] 13.4 `layer_digest.h` stays under `src/`, so it is not installed and
      carries no ABI

## 14. The cost claims, and what holds each one

- [x] 14.1 A cache changes what evaluation COSTS and never what it produces —
      held by 9.1, as parity against a full walk at both ends of the file
- [x] 14.2 Deleting every entry is flushing a CPU cache: slower, identical
      output — held by 9.7, which runs the same assertions with no cache at all
- [x] 14.3 `begin()` is no longer O(model) — held by 10.1 as a count of zero,
      not as a duration
- [x] 14.4 A dab's working set follows the brush — held by 10.2 and 11.2, as
      counts that are equal across a 400-item and a 300-item difference in
      unrelated model, so they hold on a loaded CI machine as firmly as on an
      idle one
- [x] 14.5 A stale prefix is never served — held by 9.3, 9.4, 9.5, including the
      shared-instance case that a pointer digest gets wrong
- [x] 14.6 The lazy commit's distance from the path it replaces is 0.073 of a
      cell — held by 10.4 as a measured bound
- [x] 14.7 MEASURED: the cache's own benchmark. §17.1 — the numbers in §1.4 are
      the "before" and there is no gated "after" yet

## 15. Proving the tests

- [x] 15.1 REVERT A: `SdfSourceField::fill_points` seeds from the volume
      unconditionally, dropping the coverage check. 9.1 and 9.2 fail by ~14
      cells — the whole far-bound argument, as an assertion
- [x] 15.2 REVERT B: the prefix is baked WITH redistance. 9.1 fails by 0.063 on
      the lattice, two cells, where it should be float rounding
- [x] 15.3 REVERT C: the prefix is baked over its own padded bounds instead of
      the layer's region. 9.1 fails by 0.0074 — the lattice-origin case, which
      looks right and is off by a fraction of a cell everywhere
- [x] 15.4 REVERT D: `find` trusts invalidation and skips the digest re-check.
      9.4 and 9.5 fail; the shared-instance case is the one that matters,
      because no command reaches this layer at all
- [x] 15.5 REVERT E: `layer_prefix_fingerprint` digests the whole layer rather
      than the first `count` roots. 9.3 fails — every append throws away a valid
      cache, which is the reason the two digests are separate
- [x] 15.6 REVERT F: `dependency_region` uses a brick EDGE instead of a
      diagonal. 10.3's in-band bound fails on the seam at a brick face
- [x] 15.7 REVERT G: `materialize_region` stores only bricks whose samples look
      near the surface, instead of force-storing. 10.1 fails: a re-asked brick
      is materialized again, because stored-ness stopped being the record of
      what has been filled in
- [x] 15.8 REVERT H: `preview_delta_take` drains what fits on a short buffer.
      11.1 fails — the second take is short by the bricks the first stranded
- [x] 15.9 REVERT I: the preview generation is bumped on every update rather
      than on every update that CHANGED the preview. 11.3 fails
- [x] 15.10 Each revert COMPILES under `-Werror` before its result is believed

## 16. Gates

- [x] 16.1 `cmake --build build/cpu-only`, `ctest --preset cpu-only` — green;
      `pyclay_pytest` fails identically on `main` (an import quirk of the local
      anaconda interpreter) and is excluded rather than claimed
- [x] 16.2 `check_layering.py` (the table gains `session -> eval`),
      `check_c_abi.py` hygiene + the ctypes FFI exercise,
      `check_binding_parity.py`, `check_doc_latency.py`,
      `check_kernel_dialect.py`, `check_licenses.py`, `check_swift_package.py`,
      and `release_check.py`'s version gate
- [x] 16.3 `openspec validate add-sdf-prefix-cache --strict`
- [x] 16.4 Version bump 0.59.0 -> 0.60.0 in CMakeLists.txt, clay.h
      (`CLAY_ABI_MINOR`) and pyproject.toml. A MINOR bump: the ABI only gains
      entry points and one array-element struct, and every existing declaration
      is untouched
- [x] 16.5 THE DOCS. `docs/07-brushes-and-features.md` §3.1 (Smooth is lazy —
      what `begin` now does, the dependency halo, and the one semantic
      difference with its measured size) and its new section on the prefix cache
      (the split without the loss, the far-bound rule, and that deleting the
      cache is a performance event); `docs/05-claycore-library.md` §11 (the
      incremental preview delta, the two-call drain, and why a short buffer
      takes nothing)

## 17. Remaining

- [x] 17.1 THE BENCHMARKS, with gates in `tools/check_bench.py`. Four families,
      and each one is a claim in §14 that currently has only a unit test's
      counter behind it:
      - the prefix pair — one dab through an accelerated source against the
        same dab through the full walk, at the 200 / 5,000 / 50,000 item sizes
        §1.4 measured, gated as a RATIO so it holds on a shared machine;
      - `SdfPrefixCache::build` on its own axis, since it is the cost a host has
        to find somewhere to put and nothing else in the change measures it;
      - `begin()` again, against `BM_SdfSmoothTransactionBegin`'s recorded
        number, so "no longer O(model)" is a curve and not one assertion;
      - the delta drain against `clay_sdf_smooth_preview_item`, which is the
        per-frame cost P0-3 named
- [x] 17.2 A HOST-SIDE SCHEDULING NOTE once the build benchmark exists.
      `SdfSourceField::open` never builds, deliberately, which leaves "when
      should a host call `build`" answered only in prose. It wants a measured
      recommendation, not a guess, and the benchmark above is what makes one
      possible
      CLOSED by `expose-the-prefix-cache`. The recommendation is arithmetic and
      not a feel: `build / (full dab - accelerated dab)` is UNDER TEN COLD
      WINDOWS at every size measured — 6.6 and 9.1 spread, 6.8 and 8.4 piled —
      so a host builds between gestures whenever the artist is likely to take
      more than about ten dabs into untouched windows. docs/09 also corrects the
      framing that was there: "about 870 hits" prices a build against a HIT, and
      the scheduling question is what a hit SAVES. And the pair gives a fact
      neither half gives alone — the BUILD follows the history (576 -> 2,170 ms
      for 4x the items) and the CACHE SIZE does not (268 bricks, 1.4266 MiB at
      both), so a budget is sized from the model and a build scheduled from the
      history
- [x] 17.3 THE CACHE ACROSS THE ABI. Deliberately not guessed at here: a cache
      is a session's policy and a device's memory ceiling, and the C shape for
      that (who owns it, whether it is per-document or per-host, how a budget is
      expressed) is a design question this change does not need to answer to
      ship. The preview delta is the only C surface in it
      CLOSED by `expose-the-prefix-cache` (ABI 0.79.0), and none of the three
      questions needed inventing. `clay_brick_cache` had answered all three and
      its header states the rule: "a cache belongs to whoever made it, never to a
      document". So: the HOST owns it, it is PER-HOST, and the budget is BYTES.
      The one thing that did need deciding was where the SAMPLING comes from,
      because a prefix is keyed on resolution and one built at a cell size the
      gesture does not use is a silent miss — so the three cache knobs grew onto
      `clay_sculpt_policy` beside the `cell_size` already there, mirroring the
      C++ nesting, and there is nowhere to put a second.
      It also found that ZERO means opposite things inside the library:
      `SdfPrefixPolicy::max_bytes == 0` is OFF and `SdfPrefixCache` reads 0 as
      UNBOUNDED (which is what lets a default-constructed cache in a benchmark
      hold everything). The ABI enforces OFF at the boundary rather than changing
      the library under a benchmark that depends on the other reading
- [ ] 17.4 A BOUNDARY INSIDE A GROUP. `prefix_boundary_for` returns a root count
      precisely so this stays reachable: the fold is at top-level root
      boundaries and a group is one root
