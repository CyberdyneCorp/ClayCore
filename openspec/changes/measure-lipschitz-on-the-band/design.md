# Design: why a stored-brick walk sees every pair

## The claim

`measure_sample_lipschitz()` reports the steepest difference between
neighbouring stored samples, over the cell size. The old traversal found those
pairs by walking the whole bounding lattice and asking `sample_at()` for every
point of it. The new one walks the stored bricks and reads `data_` directly.
The two see **the same set of pairs**, and the reason is the halo.

## The argument

A brick holds `kBrickDim + 1` samples per axis: the eight it owns plus one
shared with the next brick along. So brick `b` covers global sample
coordinates `[8b, 8b + 8]` inclusive, at locals `0..8`.

Take any forward pair `(g, g + 1)` along an axis. Let `b = g / 8` and
`l = g % 8`, so `l` is in `[0, 7]`. Then:

- `g` sits at local `l` of brick `b`, and
- `g + 1` sits at local `l + 1` of brick `b`, which is at most `8` — the halo.

So **every forward pair lies wholly inside one brick**, and that brick is
`g / 8`. Never split across two.

Now the two directions:

- **Brick `b` is stored.** Both ends are in its block; the new walk compares
  them. The old walk found both through `sample_at` and compared them too.

- **Brick `b` is not stored.** The only other brick that can hold `g` is
  `b - 1`, and only when `l == 0`, at *its* halo. But `g + 1` has
  `(g + 1) % 8 == 1`, so `g + 1` lives in brick `b` alone. It is not stored, so
  the old walk's `if (!next) continue` skipped the pair as well.

Equal in both directions, so the value cannot move. It does not, and
`test_volume.cpp` holds it against the old traversal kept as an oracle rather
than against a number written down once.

## Why the halo pair is the one that can be dropped

The failure mode this invites is a sweep that treats a brick as `kBrickDim`
samples per axis rather than `kBrickDim + 1` — natural, because eight is what a
brick *owns*. That drops exactly the pairs `(7, 8)`, which is where a step on a
brick boundary lives.

Silent, too: the dropped pair's lower end is still compared against its own
neighbours, so the answer stays plausible. It just stops being the maximum.

The regression test plants a field whose steepest pair is `(7, 8)` and whose
second steepest is `(8, 9)` — one sample further along, at locals 0 and 1 of
the next brick, which such a sweep *would* see. Missing the halo therefore
turns 2.5 into 1.5 rather than shading it.

## Why halo duplicates cannot disagree

Settling a pair inside one brick is only sound if the two stored copies of a
shared sample hold the same value. They do, by construction on both paths that
write samples:

- `sample_blocks` positions samples as `bx * kBrickDim + x`
  (`BrickGrid::sample_position`), so brick `b` local 8 and brick `b + 1`
  local 0 are the same integer through the same float arithmetic, and the
  callable is asked at one position.
- `rewrite` is a function of the GLOBAL coordinate, so both copies of a shared
  sample are handed the same question — which the header already states as the
  reason halo duplicates cannot drift.

## What was rejected

**Caching a per-brick maximum and updating it incrementally.** That is the
right shape for a live stroke that edits a few bricks per dab, and the wrong
shape for this change: it adds state to a volume that currently has none of it,
and it has to be invalidated correctly by every writer. The traversal fix needs
no state and captures most of the cost. The incremental form belongs with the
region-limited relax work, where there is a dirty brick set to drive it.

**Threading it.** A max reduction over the pool would have hidden the real
defect — that the walk was asking about six million points that do not exist —
behind more cores.

## Verification

- Exact equality with the old traversal across three resolutions of a
  sample-scale-ripple field and two steep fields, in `test_volume.cpp`.
- The planted halo pair, same file.
- The existing declared-bound assertions in `test_consolidate.cpp` are
  untouched and still pass: the value they check is the same value.
- Full unit suite, and the `BM_Consolidate*` pair, which gates the batched bake
  against the serial one and still holds.
