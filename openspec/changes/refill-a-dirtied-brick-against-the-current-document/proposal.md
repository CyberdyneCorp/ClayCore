## Why
Issue #665: appending an `Op::Intersect` item and then moving that same item with a uniform transform, with no refill between, left the brick cache holding the un-intersected layer. Marking the item's whole influence bound dirty and refilling did not repair it. The document was right (`clay_raycast` missed outside the cutter) and the bricks were not.

Measured on the issue's fixture (unit sphere, cylinder r 0.25 / half-height 1.6 moved to y 0.9, dim 8, voxel 0.02, band 3 voxels):

| variant | surface bricks before | after |
|---|---|---|
| cutter built at y 0.9 in one add (reference) | 192 | 192 |
| add at origin, uniform move, refill | **1171** (the sphere) | 192, bit-identical to the reference |
| the same, refilled a second time | **1171** | 192 |
| add a local item on the sphere, move a DIFFERENT item far away, refill the layer | **1123** vs 1116 | 1116, bit-identical |

The last row is not in the issue. It is the same defect without an intersect, and it shows that the cause is not the #471 swept bound.

## What the issue's reading got right and wrong
The issue guessed that the append's pending state is lost when the move calls `touch_region_from` at the appended ordinal. The loss is real, but the ordinal and the frontier path are not the cause. Instrumenting `touch_region_locked` showed every stale brick's seed at revision 3 (before the append) advanced straight to revision 5 (the move). An append re-stamps no seed, because the append log is what carries a seed across appends. The region invalidation that followed forgot the log and then advanced every seed its box missed to the new revision, including seeds still waiting to be carried across the append. After that, the refill's `rev == now` shortcut returned the pre-append value on every refill, however often the host dirtied the brick. All three region fronts share this loop (`touch_regions`, `touch_region_structural`, `touch_regions_from`), so any edit kind can do it. An intersect exposes it because its append changes the whole layer, and the swept move box is intentionally much smaller than that.

## What Changes
- The append log keeps each append's reach (the `command_influence_bound` union `apply_edit` already computes) beside the node it names, plus a running union of those reaches.
- A region invalidation advances an unreached clean seed only if (a) it was current just before this edit, or (b) the log covers every revision the seed lags by and no append in that span reached the seed's cull region. The log's union rejects most bricks in O(1) before any per-append scan. Any other seed stays at its old revision, so the next refill of that brick walks it in full.
- All three region fronts now read the log before forgetting it.

## Impact
No ABI, format or version change. A brick the host dirties is refilled against the current document whatever edits came before. The carry-forward that makes an edit after a stroke cheap is kept: a seed that no append reached is still advanced and still resumed. A regression test pins this with `clay_document_resume_stats`. A mutant that drops every lagging seed turns the resumed count to zero.
