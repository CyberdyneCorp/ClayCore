# Proposal: a dab's cost should not grow with everything already drawn

## Why

A brush dab dirties a flat number of bricks — 22 to 24, measured, whether the
document holds 100 items or 2400. That was the design's premise and it holds.

Its *time* does not hold. Over that same range a dab goes from 2.6 ms to
8.8 ms, and the reason is one line:

```cpp
// bindings/c/clay_c.cpp:5009 — inside clay_brick_cache_eval_requests
scene::CullRegion cull{request_brick_box(requests[i]).dilated(band)};
scene::Tape tape = scene::compile_document(doc->doc.document, &cull);
```

`compile_document` walks **every node in the document** to decide which few
intersect one 8³ brick. The dab pays that walk ~24 times, once per brick, and
the walk costs ~64 ns per item per brick — measured, stable across a 24× range
in document size.

The arithmetic is the whole proposal:

| Document | Culling alone, per dab |
|---|---|
| 100 items | ~0.15 ms |
| 2 400 items | ~3.6 ms |
| 10 000 items | **~15 ms** |

The interactive budget is 4–8 ms per Pencil event. At 10 000 items — a normal
afternoon's sculpt, not a stress test — the engine is over budget **before it
evaluates a single sample**, on culling that exists to make evaluation cheap.

The tape cache cannot help. It memoises the *whole-document* tape keyed on a
revision, and consecutive bricks in a dab want twenty-four different cull
regions, so every one of them misses by construction. Fanning the loop out over
workers buys about 2× — it lowers the constant and leaves the slope exactly
where it is. The slope is the bug.

## What changes

The compiler stops asking every item whether it touches the region, and asks a
spatial index instead: **built once per document revision, shared by every
brick in the dab, discarded when the document changes.**

The index is over item influence bounds, which already exist, are already the
authority on culling, and already have a single definition
(`item_influence_is_local` / `item_influence_bound`, scene-model: "Whether an
item may be culled has a single definition"). This change adds no new notion of
what an item reaches; it adds a way to ask the question without a linear scan.

`clay_brick_cache_eval_requests` also stops being a serial loop on the caller's
thread: it dispatches its requests through the same pool every other batch uses.
That is the 2× the measurement already recorded, and it composes with the index
rather than substituting for it.

## What it is not

**Not a change to what any tape contains.** The existing gate — a culled tape
gives bit-identical brick data to the full tape — is the acceptance test for
this change, unchanged and now run over a document large enough for the index to
matter. An index that returns a superset of the truth is correct but slow; one
that returns a subset is a wrong field, so the test is "identical", not
"similar".

**Not incremental tape compilation.** Adding one stamp still rebuilds the tape.
That is a bigger change with its own invalidation argument; this one only stops
the *cull* from rebuilding knowledge it could have kept.

**Not a spatial index over anything but items.** Bricks are already keyed by
lattice coordinate and need no help finding each other.

**Not a new cache the caller has to manage.** The index is owned by the same
document handle that owns the tape cache and invalidated by the same `touch()`,
so it cannot go stale independently of the tape it culls for. A second
invalidation rule is exactly the bug this codebase already avoided once by
keeping one definition of the local test.

## Open questions

- **Which structure.** A flat BVH over item bounds, rebuilt per revision, is the
  obvious candidate: build is O(n log n) and a query is O(log n + hits). A
  uniform grid keyed like the brick lattice is cheaper to build and degrades on
  a document with items at wildly different scales — which a sculpt has, since a
  blockout sphere and a detail stamp coexist. To be decided and recorded in
  `design.md`.
- **What non-local items do.** An item whose influence is unbounded (the
  non-local combine modes, the unbounded plane, infinite cylinder) cannot live
  in a spatial structure. They belong in a small always-emitted list, and the
  index answers for everything else. This must be derived from
  `item_influence_is_local` rather than re-tested.
- **Whether the index survives a partial edit.** A stamp adds one item and
  invalidates the whole index today. Whether a rebuild-per-revision is fast
  enough at 10 000 items, or whether the index needs incremental insertion, is a
  measurement to take before choosing.
- **Where it lives.** `scene::` owns compilation and is the honest home. The
  brick cache never sees a `Document` and must not start.

## Impact

`scene-model` gains the index as a stated property of culling. `c-abi` gains the
statement that a request batch is evaluated as a batch. No public signature
changes, no output value changes, and a document with few items behaves exactly
as it does today.
