# Proposal: an intersect is bounded by its layer, not by everything

## Why

`item_influence_bound` reports `Everything` for any op that is not local, and
"not local" covers two things that behave differently: an INTERSECT and the two
SPATIAL MORPHS. One of them has a finite answer.

Measured by the reporter of #319 on CUDA at 0.39.0, same object, same drag, same
scene, only the operation differing:

| operation | mean per drag frame | p95 |
|---|---:|---:|
| `CLAY_OP_SUBTRACT` | 19.39 ms | 18.31 ms |
| `CLAY_OP_INTERSECT` | **35.52 ms** | 34.46 ms |

None of that is the intersect being harder to evaluate — it is the same shape
over the same samples. It is the difference between refilling the box the shape
reaches and refilling the whole cache. One of fourteen combine operations is
categorically more expensive to drag and nothing in an interface can explain why.

## What was blocking it, and why that is now cleared

#326 held this back on a real objection: **nothing in the suite could tell a
correct bound from a wrong one here.** The item's own geometry box is ~3x tighter
than the layer's and measured drift 0 as well, so shipping the layer bound would
have meant choosing a bound 3.4x looser than one the evidence treated
identically. Four candidate fixture designs were proposed to break the tie.

**None of them was needed. The fixture was never the problem — the sample count
was.** Moving the intersect of #319's own sphere+box document leaves 34 drifting
points in 400,000, about 1 in 11,700, so the property test's 4,000 samples miss
it roughly seven times in ten. At 200,000 it shows every run:

| candidate bound | worst band-clamped drift |
|---|---:|
| the item's own geometry | **0.100** (146 points) |
| the LAYER's extent (#319's ask) | **0** (0 points) |

against a band of 0.15, comparison by exact equality, boxes dilated by
`band + cull_pad` exactly as the property test dilates them. So #319 did not ask
for a bound 3.4x too loose: it asked for the tightest one that holds, and the
tighter-looking alternative leaks.

## The split

- **An INTERSECT is bounded by its LAYER.** `max(acc, item)` can only take
  material away, and what it takes away is inside what the layer already
  occupies — it cannot put material where the layer has none.
- **A SPATIAL MORPH is not.** Its weight SATURATES:
  `ctransition_radial_weight` is `clamp((length(p.xz) - r0) / (r1 - r0), 0, 1)`
  about the WORLD Y axis, so past `r1` the weight is exactly 1 and the result IS
  the item's own field, arbitrarily far from anything the layer occupies.
  Measured: 0.0178 of drift outside the layer's extent over 4 points in 200,000.
  These keep `Everything`, and #319's report — which lumps "intersect, the
  spatial morphs" together — would have been unsound if taken literally.

Also unbounded, as before and for their own reasons: an infinite grid repeat and
a primitive with no finite extent. An intersect that ALSO repeats infinitely is
unbounded; the weaker answer must not win.

An intersect in a layer that holds something unbounded is bounded by that — a
layer holding a plane extends everywhere, so its intersect does too.

## What this does NOT change

**The CULL gate.** `item_influence_is_local` still reports false for every
non-local op, so per-brick culling still cannot drop an intersect from a tape.
The two uses were always different questions — "may this be omitted from a
brick's tape" and "which bricks does moving it dirty" — and only the second one
has the finite answer. Keeping them separate is what makes this safe.

`item_own_influence_bound` also keeps the infinite answer. It asks how far the
item's own body reaches, which is what a brush that has already reflected itself
tests a drag against; there the honest answer for a non-local item is still
everywhere, because the caller is deciding whether to warp the item at all
rather than which bricks to redraw.

## Measured

The reporter's own case, bricks dirtied per drag frame over 20 frames, a
0.3-radius operand in a layer holding a unit sphere, 1,000 tracked bricks:

| | before | after |
|---|---:|---:|
| `CLAY_OP_SUBTRACT` | 64 | 64 |
| `CLAY_OP_INTERSECT` | **1,000** | **216** |

4.6x fewer bricks, and 216 is exactly the figure the triage on #319 predicted.
Subtract is untouched.
