# Design: why identity-outside-the-region is a precondition and not a hint

## The two things it buys

`rewrite_region` writes the samples of bricks that meet the region and leaves
every other brick alone. That is the same answer as `rewrite` for two separate
reasons, and both need `fn` to be the identity outside the region.

**The skipped samples.** A brick that is not selected keeps its old values. That
is only the answer `rewrite` would have produced if `fn` would have returned
those values anyway.

**The shared samples, which is the one that bites.** A sample on a brick face
lives in *every* brick that shares it — up to eight at a corner — and the halo
is what makes a lookup inside a brick a single array read. `rewrite` cannot
break that, because `fn` is a function of the GLOBAL coordinate, so every copy
of a shared sample is handed the same question and gets the same answer.

`rewrite_region` writes only the copies held by *selected* bricks. If `fn`
changed a sample where one sharer was selected and another was not, the two
copies would drift apart, and `eval` would step at that brick face.

It cannot happen, and the argument is short: a brick that was not selected does
not meet the region. Every sample it holds lies in its own box. So every sample
it holds lies outside the region, where `fn` is the identity — and the selected
sharer wrote the same value it already had.

## Why the stencil needs no margin

This looks wrong at first, and it is worth being explicit because the obvious
implementation adds `radius_cells * iterations` of margin "for the taps".

Relax reads its taps from `previous`, which is a whole copy of the volume taken
before the pass and never written. So a sample *inside* the region may read
neighbours *outside* it freely, and they hold exactly what they held.

What the region bounds is where values **change**, and that is precisely where
the weight is non-zero — `region_weight() * mask_gate()`, the `weight <= 0.0f`
line that already returned `here`. That set does not grow across passes: a
sample outside the region is unchanged after pass 1, so pass 2 asks the same
question of the same value and leaves it alone again.

A margin would therefore cost work and change nothing. The one margin that IS
applied is a whole brick on each side of the selection, and that is only because
the selection rounds outward to brick boundaries.

## What the region is

Not the brush radius. `relax` widens a falloff narrower than the kernel —
"a taper narrower than the kernel cannot hide the seam the kernel makes" — so
the weight is non-zero out to `region_radius + tuned.falloff`, and the region
must be built from the tuned value after that clamp, not from the setting the
caller passed.

An AABB of that sphere, not the sphere: the selection is whole bricks anyway.

## The selection is conservative, and that has a consequence for testing

Brick `b` spans `[origin + b*brick, origin + (b+1)*brick]` on an axis, the
closing face being the halo shared with `b+1`. The bounds are computed by
`floor` and then rounded outward by one more brick on each side.

A brick too many costs a brick of work and cannot break correctness — the
argument above only needs "unselected implies outside the region", and a
*larger* selection preserves that.

But it means a region merely a little too small is silently absorbed. The
parity test found this the hard way: a first attempt at a negative control
passed a region less than half the width of the acting set and still got a
byte-identical result, because the outward rounding covered the difference.
Only breaking the precondition outright — an `fn` that acts everywhere — makes
the comparison fail. The test carries that control, because a parity test that
cannot fail is not a test.

## What was rejected

**A brick-list overload.** `rewrite_bricks(span<BrickCoord>)` was the original
sketch. Relax has an AABB, not a list; the selection is conservative regardless;
and a list only earns its keep once something maintains a dirty set to supply
it. That is the live-session work.

**Expanding the region per pass.** See above — it is the natural mistake and it
buys nothing.

**Fixing the far-bounds rebuild here.** `shrink_band` still rebuilds every
brick's far bound, which is now 0.571 ms of a 2.234 ms dab. The local form is
sound but *not* byte-identical to the global one, because the global rebuild
recomputes `max(chamfer, floor)` and only the floor moved — so an exact local
update has to know which entries were floor-pinned, which nothing records.
That is a design question with real options, and it is #278 rather than a
footnote here.

## Verification

- `rewrite_region` against `rewrite` for an `fn` that is identity outside four
  boxes, at two cell sizes, comparing `serialize()`. The boxes are deliberately
  off the brick lattice: an aligned box would never place a written sample and
  an unwritten one on the same brick face, which is the case the whole argument
  is about. Plus covers-everything and meets-nothing.
- The negative control described above, in the same test case.
- A region-limited relax over three passes leaves every sample beyond the taper
  equal to the input's, and changes something inside — so the first half cannot
  pass by relax having become a no-op.
- Full unit suite, 1413 cases.
