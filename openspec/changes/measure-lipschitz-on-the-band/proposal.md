# Proposal: measure the Lipschitz where the samples are

## Why

`FieldVolume::measure_sample_lipschitz()` walked the whole bounding lattice and
asked `sample_at()` for every point of it. For a bake at an interactive cell
size that is **8.1M points for a volume storing 1.55M samples in 2,132 of
15,625 brick slots**, and each of those points costs a sparse localize plus up
to eight brick probes. Roughly six million of them return nothing.

Measured on a 12-core Mac, Release, a bumpy sphere over a 2³ box:

| cell | stored samples | bake | of which the measurement |
|---|---:|---:|---:|
| 0.05 | 67,068 | 2.36 ms | 0.99 ms (**42%**) |
| 0.02 | 403,866 | 29.7 ms | 9.86 ms (**33%**) |
| 0.01 | 1,554,228 | 183 ms | 56.8 ms (**31%**) |

A third of every bake, and the bake is the one cost on the SDF Smooth path a
host cannot avoid — `field::relax`, `field::flatten`, `brush::mask_extrude`,
`field::redistance` and `scene::bake_layer` all measure what they produced.

The sparsity is the whole design of the storage — the spec already says storage
is proportional to surface area rather than volume — and `rewrite()`, two
functions above in the same file, already walks it that way.

## What

`measure_sample_lipschitz()` walks the stored bricks and reads `data_`
directly: three strided sweeps per brick, each stopping one sample short along
its own axis so the forward neighbour is always in the block. No lookups at
all.

The two traversals see the same pairs, because a forward pair `(g, g+1)` lies
wholly inside brick `g / 8` at locals `(g%8, g%8+1)` — the upper end is at
worst the halo sample. See `design.md` for the argument in both directions.

The returned value does not change, and the tests hold that rather than
assuming it. One keeps the old dense traversal as an independent oracle and
requires **exact** equality — both take a max over differences between the same
stored floats, so there is nothing to tolerance. The other plants the steepest
pair on a brick's halo sample, which is the one pair a stored-brick walk can
drop, and drops it into a value change rather than a shading.

## Impact

Measured, same machine and fields:

| cell | measurement | | bake |
|---|---|---|---|
| 0.02 | 9.58 ms → 1.30 ms | 7.4× | 29.7 ms → 21.4 ms |
| 0.01 | 53.6 ms → 4.53 ms | **11.8×** | 183 ms → **133 ms** |

The phase falls from 31% of the bake to 3%.

In-repo gate: `BM_ConsolidateGrownDoc` 54.0 → 47.4 ms.
`BM_ConsolidateSerialGrownDoc` barely moves (407 → 404), because it is
dominated by point-at-a-time tape evaluation — which is issue #271, not this.

On the reference iPad (iPad15,5, iPadOS 26.5.2), full 59-case run from a clean
tree, `valid: true`, thermals nominal at both ends — p95 at 1000 stamps:

| case | before | after | |
|---|---:|---:|---|
| `sdf_flatten` | 6.677 ms | 3.153 ms | **2.12×** |
| `volume_hpolish` (4 passes) | 149.673 ms | 72.170 ms | **2.07×** |
| `sdf_consolidate` | 340.462 ms | 314.686 ms | 1.08× |
| `mask_extrude` | 4442.964 ms | 4286.490 ms | 1.04× |
| `sdf_relax` | 1.661 ms | 1.669 ms | flat |

`sdf_relax` is the control, and it is why the rest is believable: relax is the
one verb in this group that rewrites samples without re-measuring the bound, so
it had nothing to save and saved nothing. Every case that calls
`measure_sample_lipschitz()` moved; every case that does not — including all
three SDF stamp cases — stayed flat. How far each moved tracks how much of that
verb the measurement was: flatten's own arithmetic is cheap and the measurement
dominated it, while consolidate's bake is dominated by tape evaluation instead.

The gate against the committed baseline passes with no case over budget.

## Non-goals

**A cached per-brick maximum, updated incrementally.** That is the right shape
for a live stroke editing a few bricks per dab and the wrong shape for this: it
adds state to a volume that has none, and every writer has to invalidate it
correctly. It belongs with the region-limited relax work (#272), where there is
a dirty brick set to drive it.

**Threading the measurement.** More cores over six million points that do not
exist is not a fix.

**The other two O(field-size) costs on this path.** The serial C ABI bake
(#271) and the whole-band relax traversal (#272) are separate changes, in that
order, and the profile should be re-taken after all three rather than predicted
now.
