# Tasks: speed-the-interactive-path

## 1. The tape compiler stops computing each bound twice

- [x] 1.1 `scene::item_influence_is_local` in `bounds.h/cpp`: the ONE definition
      of the non-local test, with `item_influence_bound` rewritten in terms of it
- [x] 1.2 `Compiler::compile_list` computes `item_geometry_bound` once and uses
      it for both the cull test and `tape.bounds`
- [x] 1.3 The cull test is skipped entirely when there is no cull region, which
      is every whole-document compile
- [x] 1.4 Test: the predicate and the bound agree on every non-local kind; a
      non-local item survives a distant cull region; a purely local document
      culls to nothing; a cull region covering everything yields a tape
      identical to no cull region at all
- [x] 1.5 Mutation-proven: making the predicate always answer "local" fails the
      test at both the predicate assertion and at `culled.empty()` — the
      silent-corruption case

## 2. The thread pool can rebalance

- [x] 2.1 `parallel_for` over-decomposes to `kChunksPerWorker` chunks per worker
      so the atomic claim counter has something to balance with
- [x] 2.2 `min_chunk` still floors the chunk size, so a small batch is
      unchanged and no chunk is smaller than the claim that fetches it
- [x] 2.3 Test: point evaluation covers the batch exactly at 17 sizes chosen to
      straddle both `min_chunk` values the CPU backend passes (16 and 256),
      exact multiples of the chunk count, and primes. The output is poisoned
      first so an element nobody wrote is detectable rather than accidentally
      right.
- [x] 2.4 ThreadSanitizer clean over the threaded paths (`setarch -R` needed
      locally: TSan hits an ASLR mapping conflict otherwise)

## Measurements

Desktop, Release. An ARM SoC will differ in magnitude; the shape holds.

| | before | after |
|---|---|---|
| `raycast_many` 64×64 (4 096 rays) | 13.02 ms | 7.09 ms (−46%) |
| `raycast_many` 128×128 (16 384) | 42.38 ms | 24.18 ms (−43%) |
| `raycast_many` 256×256 (65 536) | 157.80 ms | 77.79 ms (−51%) |
| `eval_points` 16 384 | 12.50 ms | 10.65 ms (−15%) |
| `eval_points` 100 000 | 62.01 ms | 56.35 ms (−9%) |
| `clay_eval_points` 1 pt, 2 680-node sculpt | 0.533 ms | 0.423 ms (−21%) |
| `clay_raycast` 1 ray, same sculpt | 1.080 ms | 0.956 ms (−11%) |
| `BM_EvalPoints` (committed benchmark) | 33.7 M items/s | 43.6 M items/s |

Note which fix each row belongs to: the `raycast_many` and large `eval_points`
rows are the pool, the single-point rows are the bound. A single point or a
single ray never threads at all — it is below `min_chunk` — so the pool change
does nothing for one Pencil event in isolation and everything for the preview
frame.

## Found while building

- [x] 3.1 `eval_points` at 4 096 shows no gain (3.61 → 3.72 ms, inside noise):
      with `min_chunk` 256 and 24 workers the batch is already at the floor, so
      over-decomposition cannot engage. This is the documented limit of the
      change rather than a regression, and lowering `min_chunk` to chase it
      would trade a real cost — dispatch is per call, so chunks worth less than
      their claim lose — for nothing.
- [x] 3.2 The bound fix removes the SECOND of two identical calls; it cannot
      remove the first, because `tape.bounds.expand` needs the geometry bound on
      every compile. The audit that found this reported "33% of compile" by
      counting both copies; the recoverable half is about 16% on a flat document
      and more on a curve-heavy one, which is what the single-point rows show.

## Deliberately not done

- The tape is still recompiled on every read. That is the larger cost on this
  path — 1.86 ms at 10 000 items against 0.20 ms to evaluate a point — and
  caching it, or better compiling incrementally so one added stamp does not
  rebuild the document, needs its own invalidation argument and its own change.
- `BrickCache` is not reachable through the C ABI (no references in
  `bindings/c/`, and `tools/build_xcframework.sh` ships only `clay.h` plus the
  kernel headers as shader source). Three separately measured wins of 7–12x sit
  behind exposing it, including the per-brick compile that re-derives every
  item's bounds for every brick. That is the biggest item on the list and it is
  an API change, not an optimisation.
