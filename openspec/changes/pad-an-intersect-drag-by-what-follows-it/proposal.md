## Why
`clay_layer_set_transform_bound` (#471, ABI 0.90.0) reports the swept box of an intersect operand's move, dilated by a chain pad. That pad was `cull_pad` over the WHOLE layer: the largest blend in the layer, resolved at the cull's envelope. On a worked form it is the stamps' blend radius, which scales with the form, so the box followed the form rather than the cutter. ClaySpaceDesktop measured it as the whole remaining cost of an intersect drag (#666): a pad of ~0.47 around a cylinder of radius 0.25 at the reference size and ~1.48 at ten times the extent, reaching past the layer, and an intersect drag frame 1.73-2.09x a subtract one.

Every one of those stamps sits AHEAD of the operand. A combine ahead of it built the `acc` that `max(acc, item)` reads, and `acc` does not change when the operand moves. Only the combines after it read the running value the move changed.

Measured on the oracle's fixture (a sphere, 24 spiral stamps whose size and blend radius scale with the form, a cylinder r 0.25 h 1.6 appended last and dragged 1.4 across at y 0.9), bricks marked by one drag frame:

| fixture | influence union | delta, before | delta, after |
|---|---:|---:|---:|
| reference (r 1) | 900 | 540 | 256 |
| ten times the cross-section (r √10) | 15,600 | 1,152 | 256 |
| 96 stamps, operand last, r 1 | 900 | 540 | 256 |
| 96 stamps, operand last, r √10 | 15,600 | 1,440 | 256 |
| 96 stamps, operand at the head, r 1 | 1,452 | 540 (35 stale bricks) | 1,452 |
| 96 stamps, operand at the head, r √10 | 28,830 | 1,440 | 28,830 |
| 96 stamps, operand in the middle, r 1 | 1,452 | 540 (17 stale bricks) | 1,452 |

Growth from the reference to ten times the extent was x2.13 and is now x1.00: the box is the sweep, and nothing the form does reaches it.

## What Changes
- `scene::command_surface_delta_bound` (through `item_geometry_reach_in_document`) pads the operand's geometry bound by the SUM of the full blend supports of the combines that follow it: its later siblings at every level of the ancestor walk, and the children of inline groups among them, which continue the outer chain. A non-inline later group contributes its own combine only, since its children start a chain of their own. Nothing ahead of the operand is a term, and neither is the operand's own combine or seam: its geometry bound already carries both supports. The enclosing groups' supports stay where they were, in `dilate_by_ancestors`.
- `clay_layer_set_transform_bound` reports the OVERLAP of that box and the conservative influence union it was already computing. Both hold the change, so their overlap does. A long smooth chain after the operand sums to a box past the layer, and the overlap is then the influence union.
- `cull_pad`, the per-brick cull and every other bound are unchanged.

## What building it found
- The plan was the issue's option 1: keep `cull_pad`'s terms and its envelope resolution, restricted to the combines after the operand. The first fixture that put the operand AHEAD of smooth combines refuted the envelope. Every pre-existing oracle case appends the operand last, so none had ever exercised the pad. With 96 smooth stamps after it, the shipped layer-wide pad left 35 bricks (head) and 17 (middle) one fp16 step off a full rebuild. Restricting to the suffix left the same 35 and 17, and so did resolving it at the largest FULL support. The true field moves by 1e-5 to 3e-5 at points 0.3 outside the box: the chain's running value is dragged down step by step until an in-band stamp reads it. No single term bounds that.
- A sum does. A changed value above `band + s + R` entering a combine of support `s` (with `R` the supports after it) comes out either bit-identical (`|a - b| >= s`) or above `band + R`. A smooth union only reads `a` where `b > a - s` and lowers the smaller operand by less than `s`. A smooth subtract or intersect only raises it, and a feathered replace moves it by at most its band. The oracle is clean at every slot, under mirror and radial symmetry and through inline groups.
- The sum makes a long smooth suffix useless on its own (120 stamps of k 0.06 sum to a pad of 28.8), which is why the C ABI clips. Before, an operand ahead of a long smooth chain got a narrow box that was not exact. Now it gets the influence union, which is. That is a cost increase for that shape (540 -> 1,452 bricks at the reference size), and it is the price of the region being right.
- The probe in `test_intersect_delta_bound.cpp` checks a classification (sign and band membership), and it passes on the old pad at every slot. Only the bit-exact oracle sees the stale bricks. The probe now clips its box the way the C ABI does, which is a stricter check of the same claim.
- Dropping the walk into inline groups leaves 52 stale bricks when the stamps after the operand sit in one.

## Impact
An operand appended last, which is how a host adds a cutter to a sculpt, now refills its sweep alone. An operand with hard combines after it pays nothing either. An operand with smooth combines after it pays their summed supports, clipped to the influence union, where it previously got a narrower box that left stale bricks. No ABI entry point, descriptor or format changes. The C ABI version stays at 0.121.0.
