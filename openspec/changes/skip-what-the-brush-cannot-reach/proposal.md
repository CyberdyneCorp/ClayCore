# Proposal: stop paying for the samples a brush cannot reach

## Why

With every model-scaling term out of a dab, the largest thing left was not the
stencil. It was the work done *before* the stencil, on samples the brush cannot
touch.

A steady dab at a 0.01 cell, decomposed:

| | |
|---|---:|
| per-sample overhead — `here` lookup, `cell_position`, `region_weight` | **0.843 ms (56.8%)** |
| the 7 stencil taps | 0.499 ms (33.6%) |
| traversal + `std::function` | 0.124 ms (8.4%) |
| region snapshot | 0.018 ms (1.2%) |
| `build_stencil` | 0.00016 ms (0.011%) |

And most of that overhead was spent on nothing. A brush is a ball; the bricks
selected for it are a box around one:

| cell | brush radius | bricks selected | reachable | samples visited | weight > 0 |
|---|---:|---:|---:|---:|---:|
| 0.02 | 0.05 | 64 | 17 | 46,656 | 2,475 — **95% wasted** |
| 0.02 | 0.40 | 256 | 104 | 186,624 | 46,994 — 75% wasted |
| 0.01 | 0.05 | 112 | 42 | 81,648 | 14,284 — 83% wasted |
| 0.01 | 0.40 | 708 | 350 | 516,132 | 193,627 — 62% wasted |

## What

Three changes, none of which alter a single stored sample.

**The base value is not looked up.** `rewrite_region` hands the callback the
value held in the brick it is writing, and that IS what a lookup of the same
coordinate would return: the brick has not been written yet this pass, the
snapshot holds the pre-pass value, and two copies of a sample shared across a
brick face cannot disagree because every writer is a function of the global
coordinate. So the lookup went, on every sample rather than only the wasted
ones.

**The weight compares squared distances.** Both answers that need no
interpolation — inside the full-strength radius, and outside the taper — come
from the square. Only the taper itself takes a root, and it is the minority.

**The selection narrows to the ball.** `FieldVolume::Region` is a box,
optionally narrowed to a ball inside it, and `rewrite_region` and
`snapshot_region` both reject a brick the ball cannot reach. It is sound for
the same reason the region limit is at all: the operator is the identity
outside its region, so a brick the ball cannot reach holds nothing the pass may
change.

## Impact

| 24-dab stroke | before | after | |
|---|---:|---:|---|
| steady dab, cell 0.01 | 1.61 ms | **1.00 ms** | **1.61×** |
| steady dab, cell 0.02 | 0.61 ms | **0.23 ms** | **2.71×** |
| first dab, cell 0.01 | 2.39 ms | 1.65 ms | 1.45× |

The narrowing shrinks the snapshot too, since both walkers take the same
region.

## Non-goals

**The brick+halo scratchpad**, which now becomes the largest term. It attacks
the stencil taps, and should be measured against the profile this leaves rather
than the one that motivated it.

**Caching the relax stencil.** `build_stencil` measures **0.011%** of a dab. It
was P1 in the plan this program came from and has never been worth doing.

**Dropping `std::function` from `rewrite`.** At most 8.4%, shared with the
traversal itself.
