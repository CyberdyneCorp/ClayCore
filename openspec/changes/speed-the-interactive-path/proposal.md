# Proposal: two costs on the interactive path that buy nothing

## Why

The consumer is an iPad sculpting app. An Apple Pencil emits events at 120–240
Hz, so the budget per event is 4–8 ms, and a preview frame at 60 fps is 16.7 ms.
Both numbers are for a mobile SoC that pairs fast cores with efficiency cores
and is thermally limited, not for the desktop this code is benchmarked on.

Two costs on that path are pure waste — not a trade, not a cache that has to be
invalidated, just work whose result is discarded or never claimed. Neither is an
algorithmic change and neither alters a single output value.

**The tape compiler computed each item's bound twice.**
`item_influence_bound` returns `item_geometry_bound` for a local item, and the
next line asked for `item_geometry_bound` again. Both calls do the real work —
for a stroke or a sweep they re-tessellate the curve — so the second was a
duplicate on every compile, and every whole-document compile happens on the
interactive path: `clay_eval_points`, `clay_eval_gradients`,
`clay_layer_safe_step_scale`, and `clay_raycast` through `pick::pickable_tape`
each recompile the document from scratch.

**The thread pool could never rebalance.**
`chunk = ceil(n / workers)` followed by `num_tasks = ceil(n / chunk)`
mathematically forces at most one chunk per worker. The atomic claim counter in
`run()` therefore handed each thread exactly one chunk and a thread that
finished early parked instead of taking more, so every call took as long as its
slowest chunk. On a big.LITTLE SoC the cores are not interchangeable by design,
which is precisely the case the schedule could not adapt to.

## What changes

The bound is computed once. The pool over-decomposes into several chunks per
worker so the counter has something to balance with; `min_chunk` still floors
it, so a small batch is unaffected and no chunk becomes too small to be worth
claiming.

Measured on this desktop (an ARM SoC will differ in magnitude, not in shape):

| | before | after |
|---|---|---|
| `raycast_many` 64×64 | 13.02 ms | 7.09 ms |
| `raycast_many` 128×128 | 42.38 ms | 24.18 ms |
| `raycast_many` 256×256 | 157.80 ms | 77.79 ms |
| `eval_points` 16 384 | 12.50 ms | 10.65 ms |
| `clay_eval_points`, 1 point, 2 680-node sculpt | 0.533 ms | 0.423 ms |
| `BM_EvalPoints` (the committed benchmark) | 33.7 M items/s | 43.6 M items/s |

## What it is not

Not a tape cache. Every read still recompiles the document, and that remains the
largest single cost on this path: at 10 000 items a compile is 1.86 ms against
0.20 ms to evaluate a point. Caching it, and better still compiling
incrementally so that adding one stamp does not rebuild everything, is a
separate change with its own invalidation argument to make.

Not a change to what anything computes. The bound fix keeps ONE definition of
the non-local test — `item_influence_is_local`, which `item_influence_bound` is
now written in terms of — because the tempting version of this fix inlines that
three-way predicate into the compiler, where it becomes a second copy that can
go stale. If it ever disagreed, an item would be dropped from per-brick tapes
only, so the whole-document tape would still look correct and the field would be
wrong just inside bricks that do not touch the item.

Not a lowering of `min_chunk`. Chunks worth less than the claim that fetches
them are a loss, and dispatch cost is per call rather than per chunk, so the
floor stays where it is.
