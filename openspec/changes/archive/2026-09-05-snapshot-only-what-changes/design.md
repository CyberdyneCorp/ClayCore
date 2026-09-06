# Design: the read path is the whole difference

## The obvious version is slower than what it replaced

The first implementation was correct and **4% slower over a stroke** than the
full copy it removed. Worth recording, because nothing about the design says so.

A snapshot has to answer the same question `FieldVolume::sample_at` does: the
value at a global cell coordinate, where a sample on a brick face lives in every
brick sharing it. Written the obvious way — try each of the (up to eight) bricks
that could hold it, then fall through to the volume — it costs more per tap than
a sparse lookup does.

And a tap is not rare. A radius-1 stencil is seven of them per sample, and a
dab of a few tens of thousands of samples makes a few hundred thousand. Saving
one 0.162 ms copy and adding 0.3 ns to each of 300,000 lookups is a loss.

## The canonical brick

Every global coordinate has exactly one brick whose local index for it is inside
`[0, kBrickDim)`: `g / kBrickDim`. The other sharers hold it at their halo, and
only exist as an answer when the canonical brick is absent.

So the lookup tries the canonical brick first and returns from it, and a stencil
walking the inside of a region never does anything else. The eight-way search
survives underneath for the case the canonical brick was not snapshotted, and
below that the fall-through to the volume.

With that, the snapshot is 1.13× FASTER than the copy at a 0.01 cell rather than
4% slower.

## Why it beats the copy by more than the copy costs

The copy was 0.162 ms of a 1.81 ms dab — 9% — and removing it bought 11%. The
extra is the read path.

A snapshot of one brush's worth of bricks is a few hundred kilobytes and stays
in cache. A tap against the whole volume runs `locate` per axis and probes a
sparse index into six megabytes. The snapshot's fast path is three integer
divisions and a dense index into a small array.

That is also why the gain is at the FINE cell and not the coarse one: at 0.02
the whole volume is 1.5 MB and the difference in locality is small.

## Why reads outside the snapshot may go to the live volume

The snapshot holds the bricks `rewrite_region` will write. Everything else is
untouched for the pass's duration, so the volume is still its own "before" for
those coordinates — no copy needed.

This is the same argument the region limit itself rests on, and it inherits the
same precondition: the snapshot and the rewrite must name the same region. A
snapshot of a smaller region would read bricks the rewrite is part way through.
The header states it as a requirement rather than leaving it to be inferred.

## Verification

The invariant is simple enough to test directly: a snapshot taken of a region,
used while THAT region is rewritten, answers with the volume as it stood
beforehand — everywhere, not only inside the region. The test rewrites the
region to a constant, so a snapshot reading through to the live volume by
mistake comes back with the constant rather than the original, and compares
every sample of the lattice against a full copy taken before.

Four regions per cell size, deliberately off the brick lattice: an aligned box
would never put a snapshotted brick and an un-snapshotted one either side of a
shared face sample, which is the case the slow path exists for. Plus
covers-everything and meets-nothing.
