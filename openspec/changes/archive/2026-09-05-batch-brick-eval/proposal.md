# Proposal: refill a dab's bricks in one dispatch

## Why

Evaluation is **98%** of an interactive SDF stamp — 167.5 ms of a 170 ms stamp
on a 50,000-item document, against 2.96 ms for the cull index rebuild and
0.22 ms for the per-brick tape compile.

The CPU backend does thread it. It threads it badly, and the reason is
structural rather than a tuning constant.

`Backend::eval_grid_batch`'s base implementation loops `eval_grid` per brick.
`eval_grid` parallelises over a grid's **z-slices**. A brick is **8 cells
across**. So a 13-brick refill paid **thirteen dispatch-and-join barriers**, and
each of them could occupy **at most eight threads** however many the machine
has.

Measured on a 24-thread desktop, CPU time over wall time taken inside the
evaluate call:

| items | evaluation | cores used |
|---:|---:|---:|
| 100 | 1.95 ms | 8.85 |
| 1,000 | 4.55 | 6.85 |
| 10,000 | 34.2 | 6.84 |
| 50,000 | **167.5** | **6.69** |

Sixteen physical cores, and the share FALLS as the document grows.

## What changes

The CPU backend overrides `eval_grid_batch` with **one `parallel_for` over every
ROW of every brick**: one barrier instead of thirteen, and 13 x 8 x 8 = 832
units for the pool to balance instead of 8.

`eval_grid` moves from z-slices to rows for the same reason, so both paths
dispatch the same unit and there is one piece of inner-loop code rather than
two that could drift.

| items | before | after | |
|---:|---:|---:|---:|
| 100 | 1.95 ms | **0.42 ms** | 4.6x |
| 1,000 | 4.55 | **1.65** | 2.8x |
| 10,000 | 34.2 | **14.6** | 2.3x |
| 50,000 | 167.5 | **77.4** | **2.2x** |

Cores used: 6.7 to 17.9.

## What it does NOT change

**A single number.** The override is a different DECOMPOSITION, not a different
computation: every point is `tape.eval(p)` for the same p either way, and
nothing sums across points, so the results are bit-identical rather than merely
close. That is asserted directly — a 13-brick batch against the same 13 grids
evaluated one at a time, distances and colours, `==` and not a tolerance.

**Any other backend.** The GPU backends already override `eval_grid_batch` for
their own reason (one device submission rather than a round trip per brick,
issue #64). This gives the CPU backend a reason of its own.
