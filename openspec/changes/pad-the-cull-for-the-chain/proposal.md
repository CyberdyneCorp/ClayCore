# Proposal: pad a cull for the chain, not just for one blend

## Why

`CullRegion` promises band-clamped results bit-identical to the full tape
inside the region. Measured, that holds for a hard union at any chain length
and fails for a smooth union:

| items | in-band disagreements | worst |
|---:|---:|---:|
| 5 | 0 / 1588 | 0 |
| 25 | 2 / 1604 | **0.009123** |
| 100 | 38 / 1584 | 0.002056 |
| 300 | 73 / 1609 | 0.0004793 |
| 600 | 95 / 1627 | 0.0001365 |

0.009 is **half a cell** at the resolution that document bakes at, and these
are samples *inside* the band, which is where the surface is.

Hard unions measure zero at every length.

## It is a dropped contributor, not rounding

Sweeping the dilation gives a **threshold**, not a decay. The error sits at
exactly 0.0002016 from `band+0.00` through `band+0.11` and is zero from
`band+0.12` on, while the instruction count climbs smoothly throughout. One
contributor entering — not accumulated float re-ordering.

## The mechanism

An item's influence bound is dilated by `max(blend.support(), blend.k)`: what
**one** blend can move a value it is applied to. A chain is different. The
accumulated value part way down it sits well above where it ends up, so an item
whose *final* contribution is nothing can still be within `k` of the **running**
value and steer it.

Measured directly: at `k=0.06` the chain drags the field **0.26** below the base
sphere's own distance — more than four times `k` — while the same shapes hard-
unioned drag it by the dab radius (0.04) and nothing else.

That is also why the count rises and the magnitude falls as the chain grows: a
crowd of items each contributing near its own edge.

## What

The compiler pads the caller's region by the largest single-item reach in the
layer — exactly as it already pads for a feathered replace
(`feather_cull_pad`), and for the same reason: something reaches further than
its own bound.

Callers are unchanged. `BrickCache::cull_region` still dilates by the band
alone, and its comment now says why it should not do more.

Both pads come from **one** walk of the node map (`scene::cull_pad`). Each
walked every node, and the compiler asks for the total on every uncached
compile; at ten thousand items that second walk measured 20–30% on the
per-brick cull benchmarks. `CullIndex::feather_pad` becomes `cull_pad`, since
it stopped being only the feather.

## Impact

Correctness on every blended document evaluated through a culled tape — which
is the interactive stamp path, since `BrickCache` culls per brick.

The cost is real and *is* the fix rather than an accident: a wider region keeps
items that genuinely matter. 20–35% on the `DeepDocCull` benchmarks, whose
document blends at `k=0.03`. `check_bench.py` passes and no ratio gate moves.

## Non-goals

**A proof.** The pad closed every case measured, at chain lengths from 5 to
600, and it is not a bound: the drag grows with chain length and no fixed
dilation covers an arbitrary document. `tape.h` now says that rather than
implying otherwise.

**Changing what callers dilate by.** The caller's contract is unchanged and the
compiler absorbs the difference, so nothing outside `scene` has to learn this.
