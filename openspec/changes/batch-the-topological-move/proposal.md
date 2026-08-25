# Proposal: move a document's material through the pool

## Why

`bake-the-document-in-blocks` moved every document-sourced verb onto a batched
evaluator except one. `move_topological` was left because it is a different
shape, not because it was cheap:

| | nodes 193, cell 0.02 | nodes 600 |
|---|---:|---:|
| per-point | 4,605 ms | 14,245 ms |

A tape instruction costs about ten nanoseconds against one nanosecond of
arithmetic, so the interpreter is nearly all of this operation, and it was being
paid one point at a time — 2.09 million times.

## What made it different

The other two verbs evaluate their source **at the sample lattice**. Relax's
document form is sample-then-relax; flatten blends what the tape said at `p`
toward a plane. A `BrickBlockFill` — a fill that knows the grid — answers both.

`move_topological` samples at a **pulled-back** point: where an output sample
takes its material from is `p - displacement * w`, and `w` is the geodesic
weight at `p`. The query positions are not the grid's, and only the caller's
evaluator can be told where they are.

So the overload takes a batch of **arbitrary** points — packed xyz in,
distances out, the same shape `FieldVolume::ColorBlockFill` already uses.

## Measured before designing

`solve()` was the open question in #275: worth batching separately, or not?

Measured, it is **4–5%** of the operation and makes 87k of the 2.09M source
calls. It was never the point. The sampling pass is — and both go through the
one batched source anyway, because the material array `solve` walks over is a
dense box of cells with no dependency between them, which is a single batch.

## Impact

| | before | after | |
|---|---:|---:|---|
| nodes 193, cell 0.02 | 4,605.22 ms | 305.29 ms | **15.1×** |
| nodes 600, cell 0.02 | 14,245.39 ms | 894.91 ms | **15.9×** |

Byte-identical. No behaviour change, no signature change on the existing
overloads.

`solve()` splits into `make_grid` and `solve_over`, so the two ways of filling
the material array share the graph code instead of copying it, and the
pull-back becomes one function both overloads call.

## Non-goals

**A C ABI entry point.** There is no `clay_item_volume_move_topological_from`;
the C ABI's move takes an existing volume. The document-sourced form is reached
from `pyclay` only, which is why this was lower priority than the three in
#271 and is why nothing in the ABI changes.

**Batching `solve`'s walk.** The wavefront is sequential by construction — a
priority queue over a graph — and it makes 4% of the source calls. Its material
fill is batched; its traversal is not, and should not be.
