# Proposal: do not re-derive far bounds a shrink cannot have changed

## Why

`relax` ends by narrowing the band — smoothing moves the surface, and the
sample-free bricks were classified against where it used to be, so their bounds
would otherwise overstate the distance to where it is now.

`shrink_band` then rebuilt **every** brick's far bound: a two-pass chamfer over
15,625 brick slots for the 2,132 that store anything. **0.571 ms of a 2.234 ms
dab** at a 0.01 cell — a term that scales with the model, inside a brush that
`make-the-relax-dab-local` had just made scale with the dab.

Most of the time it rebuilt what was already there. What `build_far_bounds()`
derives depends on three things: the stored-brick set, the grid, and the band.
An operator that rewrites samples in place changes none of the first two. So
with the band unmoved, the rebuild reproduces the array it started from.

And the band stops moving almost immediately. A bake starts at three cells and
the floor is two, so the first dab of a stroke spends the narrowing and every
dab after it asks for one that cannot happen. Measured over five dabs at a
0.01 cell, only the first moved it.

## What

`shrink_band` computes the narrowed band, and returns without rebuilding when
it equals the band already held.

That is the whole change. It is exact rather than approximate: the guard is
that the band did not move, and when it did not, the rebuild was the identity.

## Impact

A stroke of 24 dabs dragged across the +x cap, each timed:

| | first dab | steady p50 | |
|---|---:|---:|---|
| cell 0.01, before | 2.75 ms | 2.321 ms | |
| cell 0.01, after | 2.62 ms | **1.767 ms** | **1.31×** |
| cell 0.02, before | 1.21 ms | 0.886 ms | |
| cell 0.02, after | 1.29 ms | 0.882 ms | — |

The first dab is unchanged, correctly — it is the one that narrows the band.
The 0.554 ms the steady dab drops by is the chamfer, within noise of the
0.571 ms it measures on its own. At a 0.02 cell the chamfer was already small
next to the other terms.

## Non-goals

**The per-pass volume copy**, the other half of #278. It is 0.162 ms, which was
7% of a dab before this change and is ~9% after. Removing it needs a
region-scoped snapshot — the taps read from a whole unwritten copy today, and
only the written bricks plus their tap reach actually need snapshotting — which
is a structural change rather than a guard. #278 stays open for it.

**Caching the chamfer.** #278 proposed storing per-brick chamfer steps so the
rebuild could be O(bricks) instead of a two-pass sweep. That is the right fix
for a shrink that DOES narrow, and it is unnecessary for one that does not,
which is every dab after the first. Adding an array to every volume to speed up
a once-per-stroke event is the wrong trade.
