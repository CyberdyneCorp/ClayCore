# Tasks: add-item-spatial-index

- [ ] 1.1 DECIDE and record in `design.md`: flat BVH over item bounds vs uniform grid keyed like the brick lattice. Decide against a real sculpt's scale distribution (a blockout sphere and a detail stamp coexist), and record the build-vs-query measurement that settled it

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
- [ ] 1.3 Build the index over item influence bounds, derived from `item_influence_bound` — no second definition of reach, and non-local items (`item_influence_is_local` false) held in an always-emitted list rather than indexed
- [ ] 1.4 `compile_document(doc, cull)` queries the index instead of scanning. The linear scan stays available behind a flag or a build so the equivalence test below can run both
- [ ] 1.5 Own the index where the tape cache is owned, invalidated by the same `touch()`, so index and tape cannot disagree about the same document
- [ ] 1.6 `clay_brick_cache_eval_requests` dispatches its batch through the worker pool instead of looping on the calling thread, sharing one compiled document across the batch
- [ ] 1.7 Equivalence test: over the golden corpus plus a generated 10 000-item document, every brick's culled tape gives **bit-identical** data to the full tape and to the linear-scan cull. Not "within tolerance" — a subset is a wrong field
- [ ] 1.8 Regression test for the non-local case: a document containing an unbounded item and a distant brick still emits that item
- [ ] 1.9 Regression test for staleness: add / move / remove an item, cull immediately, assert the culled tape reflects the edit
- [ ] 1.10 Benchmark: culling time per brick against document size, asserting the slope is flat rather than the constant small. A 2× constant improvement passing as a fix for this is the failure mode to guard against

      — A SECOND FAILURE MODE, from #193: the dab has to land on geometry. That
      issue's first end-to-end harness placed the dab in empty space, so the cull
      dropped every item and the tape emitted **0 instructions**. It measured the
      cost of finding nothing, and reported the rebuild as 90% of a stamp where a
      dab on real geometry makes it 1.7%. Assert the benchmark's culled tape is
      non-empty and that its instruction count grows with document size — otherwise
      the flat slope it reports is the slope of doing no work.
- [ ] 1.11 Order-independence test on the batch path: the same batch evaluated twice is bit-identical
- [x] 1.12 Update `docs/RELEASE.md`'s "a brush dab's brick COUNT is flat, but its cost is
      not" entry with what actually landed, including whatever slope remains.
      — Done ahead of the index rather than after it, because the entry's published claim
      (a 10 000-item sculpt past the interactive budget on culling alone) had already
      stopped being true and someone may have planned around it. The correction is recorded
      as a correction rather than quietly rewritten.
