# Tasks

- [x] 1.1 Make the append log multi-consumer, so tape, cull index and refill each take their own tail of one log; drop #309's duplicate
- [x] 1.2 `compile_layer_suffix` takes a cull region and the document's pad, so a suffix culls exactly as a whole-document compile does
- [x] 1.3 A bounded per-brick seed store on the document, dropped on any non-append edit
- [x] 1.4 `clay_brick_cache_eval_requests` serves what it can from seeds, gathers the rest into one batch, and keeps every result
- [x] 1.5 Gate on the pad, on the brick having had an accumulator, on distances only, and on a single visible SDF layer
- [x] 1.6 Build the checkpoint locally rather than borrowing the tape's, which is cold in exactly this path
- [x] 1.7 Tests: a stroke against a fresh document, several appends between reads, undo falling back, colour falling back — each bit-for-bit
- [x] 1.8 A guard that the bricks straddle the surface, so no comparison can pass as two readings of far-outside
- [x] 1.9 Mutation-test that the resumable path is actually taken
- [x] 1.10 Benchmark pair and `check_bench.py` gate
- [x] 1.11 Update `docs/` where the refill's cost is described
