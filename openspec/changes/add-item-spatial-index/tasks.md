# Tasks: add-item-spatial-index

- [x] 1.1 DECIDED, by building the BVH and measuring it: **a per-revision tree
      cannot pay for itself, and BVH-vs-grid is the wrong question to answer
      first.** The warning already in this task turned out to be exactly right,
      and is now a measured fact rather than a prediction — see 1.1b.
      Original question: Decide against a real sculpt's scale distribution (a blockout sphere and a detail stamp coexist), and record the build-vs-query measurement that settled it

      — MEASUREMENT TAKEN, from #193. Documents of N spheres, one layer, no groups.
      `plan()` is the mean of 200 calls over a dab-sized region; `build` is one
      `CullIndex` construction.

      | items | `CullIndex` build | `plan()` |
      |---:|---:|---:|
      | 1 000 | 0.102 ms | 0.0029 ms |
      | 10 000 | 1.074 | 0.0335 |
      | 50 000 | 3.642 | 0.136 |

      Both terms are linear. The rebuild is **27x the query it accelerates** at
      50 000 items and runs just as often: the index is cached on the document
      revision (`bindings/c/clay_c.cpp:952`) and every stamp in a stroke bumps it.
      So a structure that only makes `plan()` sublinear addresses the smaller term
      of the index's own cost, and this DECIDE has to weigh incremental insertion
      ALONGSIDE the BVH-vs-grid question rather than after it. The rebuild is
      expensive for a reason worth carrying into the decision: `build_chain` calls
      `item_geometry_bound` per node, which re-tessellates spline strokes and
      sweeps.

      Scope it honestly, though. At 50 000 items the whole rebuild is 1.7% of a
      realistic stamp, not the interactive cost — #193's own correction, after its
      first harness measured a dab over empty space. An O(N) rebuild per edit is
      still wrong, but it buys ~1.02x end-to-end and must not be sold as more.

- [x] 1.1b MEASURED, on one Linux box, ratios only. A median-split BVH over each
      chain's cullable entries was built, made correct (equivalence tested
      against `item_geometry_bound` / `item_influence_is_local` directly), and
      measured against the same build with it disabled:

      | items | build, scan | build, BVH | plan, scan | plan, BVH |
      |---:|---:|---:|---:|---:|
      | 1 000 | 0.091 ms | 0.113 ms | 0.0030 ms | 0.00006 ms |
      | 10 000 | 0.506 | 1.730 | 0.030 | 0.00017 |
      | 50 000 | 2.584 | 9.228 | 0.136 | 0.00023 |

      **The query got 590x faster and the whole thing got 2.4x slower.** `plan()`
      became genuinely sublinear — per-item cost falls from a flat 2.8 ns to
      near zero across a 300x range, which is the shape a search gives and a
      constant-factor fix cannot. It is also the SMALLER TERM, exactly as this
      task said, and the tree costs more to build than the scan it replaces
      saves: +6.6 ms of build against -0.14 ms of query at 50 000 items.

      The ratio that decides it is BUILD-TO-PLAN, and it is 1:1. The index is
      cached on the document revision and every stamp bumps it, while `CullPlan`
      exists precisely so one cull serves every brick in the dab. One build, one
      query. No tree amortises against that.

      **So the BVH was reverted rather than shipped.** What survives is the
      equivalence and shape tests, which are written against the public
      definition rather than against any implementation and will guard whichever
      index does land.

- [x] 1.1c DONE, under `extend-the-index-without-the-document` and
      `append-the-cull-index` rather than here. `CullIndex::append`
      (`src/scene/cull_index.cpp:149`) extends a live index, and
      `scene::append_cached` is the one place that decides whether the cached
      index may be extended in place or has to be copied first — it extends in
      place when no reader holds it, which is the ordinary case. An append at
      20,000 items went **0.0542 ms → 0.000258 ms** and stopped scaling with
      the document. Held by "cull index: an appended index is the index a
      rebuild would give" and "the pad an append raises is a maximum of SUMS
      over layers" in `test_cull_index.cpp`.
- [x] 1.2 Baseline the numbers this change exists to move, on a build of `main`: culling
      time per brick at 100 / 2 400 / 10 000 items, and a dab's total. Committed as
      `BM_DeepDocCullPlanned10000` and `BM_DeepDocRefillPlanned10000`, with gates.
      — AND IT CHANGED THE ARGUMENT. The proposal predicted ~15 ms of culling at 10 000
      items and "over budget before it evaluates a single sample". Measured: 0.926 ms of
      culling and 0.934 ms for the whole dab, against a 4.17 ms frame share. `CullIndex`
      and `CullPlan` landed after the proposal was written and closed the gap. The SLOPE is
      exactly as described (linear across a 52x range) and culling is still 99% of a dab's
      cost, so the direction holds — but this is not urgent, and it runs out at ~100 000
      items rather than at 10 000. The proposal's "Why" now says so.
- [x] 1.3 DONE. `CullIndex::Entry` carries the bound and a `local` flag taken
      from `item_influence_is_local`, so a non-local item always survives rather
      than being indexed. There is no second definition of reach — note that
      `bound-an-intersect-by-its-layer` later split `item_influence_bound` three
      ways WITHOUT touching that flag, precisely so this stays true.
- [x] 1.4 DONE. `compile_document(doc, cull, index, plan)` takes the index, and
      passing none still scans — so the equivalence tests run both against each
      other without a build flag.
- [x] 1.5 DONE. The C ABI owns it beside the cached tape under one mutex
      (`cull_index_locked()`), keyed on the same document revision, so the two
      cannot disagree.
- [x] 1.6 DONE, under `batch-brick-eval`. The refill hands whole chunks to
      `eval_grid_batch`, and the CPU backend spreads them over
      `parallel::ThreadPool` by ROWS rather than z-slices — a brick is 8 cells
      across, and eight slices could not occupy more than eight threads, which
      was the whole of the old batch path's scaling. MEASURED 2026-09-01 on a
      12-core M2 Max, as CPU time over wall time: **~11 of 12 cores** for a
      12-brick window at every document size from 200 to 50,000 items. There is
      no parallelism left to win on this path.
- [x] 1.7 DONE. "cull index: byte-identical per-brick tapes on the gnarly
      corpus" and "the tree returns exactly what the scan did, in the same
      order", plus the adversarial cases beside them (mirrored layers with
      spline strokes and deformer chains, groups with blend dilation and
      repeats, feathered replace chains). Byte equality, not tolerance.
- [x] 1.8 DONE. "cull index: an entry that can never be culled always
      survives".
- [x] 1.9 DONE, by the revision key plus "an appended index is the index a
      rebuild would give".
- [x] 1.10 DONE, and held as a SLOPE rather than a constant exactly as this
      task demands: "cull index: the cost is SUBLINEAR in document size, not
      merely smaller" in `test_cull_index.cpp`, with `BM_DeepDocCullPlanned*`
      as the benchmark half.

      — A SECOND FAILURE MODE, from #193: the dab has to land on geometry. That
      issue's first end-to-end harness placed the dab in empty space, so the cull
      dropped every item and the tape emitted **0 instructions**. It measured the
      cost of finding nothing, and reported the rebuild as 90% of a stamp where a
      dab on real geometry makes it 1.7%. Assert the benchmark's culled tape is
      non-empty and that its instruction count grows with document size — otherwise
      the flat slope it reports is the slope of doing no work.
- [x] 1.11 DONE, in `test_parity.cpp`: "cpu batch grids are bit-identical to
      the same grids one at a time", "a grid batch answers what per-grid
      evaluation does", and "batch dispatch covers every element exactly once,
      at every size".
- [x] 1.12 Update `docs/RELEASE.md`'s "a brush dab's brick COUNT is flat, but its cost is
      not" entry with what actually landed, including whatever slope remains.
      — Done ahead of the index rather than after it, because the entry's published claim
      (a 10 000-item sculpt past the interactive budget on culling alone) had already
      stopped being true and someone may have planned around it. The correction is recorded
      as a correction rather than quietly rewritten.

## Reconciled 2026-09-01

Every task above is done, and none of the last six was ticked when it landed —
the work went in under `batch-brick-eval`, `extend-the-index-without-the-document`
and `append-the-cull-index`, and each of those ticked its own file rather than
this one. A reader starting from this file would have rebuilt `CullIndex`.

What this change set out to move it did move: a dab's cull is sublinear in the
document, an append extends the index instead of rebuilding it, and the refill
reaches ~11 of 12 cores. What is NOT fixed, and is not this change's to fix, is
that a brick with **no seed** still walks the whole surviving edit list — that
is #306, and it is an evaluation cost rather than a culling one.
