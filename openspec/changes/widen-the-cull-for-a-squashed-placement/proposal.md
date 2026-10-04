## Why
#649: a full brick build disagrees with the document's own raw field (`clay_eval_points`) at in-band samples. #652 fixed the deformer cull, which was the largest part. It named two mechanisms that remain, and this change fixes the second (B): **squashed placements are culled as if their field were a distance.**

A per-axis scale, on an item or on its layer, makes the field a bound on the distance rather than the distance. `cscale_nu_dist` multiplies the local value by the smallest component, so the value can be short of the true distance by up to `q = max(s) / min(s)`, and the two levels' ratios multiply. The per-brick cull drops an item once its bound is more than band + pad from the brick. For a squashed item that is not enough: its field there can still be inside the band out to q times that distance. Seeds 5743 and 6290 of the rich sweep reduce to this: 5,625 and 151 in-band samples off, worst 0.068.

`item_geometry_reach_in_document` already refuses a squashed placement for the same reason. The cull cannot refuse, because exempting every squashed item would make a squashed layer cull nothing. It widens the bound instead.

## What Changes
- `scene::CullRegion` gains `band`: how far inside the region the samples lie. Its zero means what every existing `CullRegion{box}` meant.
- `scene::CullSquash` (`item_cull_squash`, `node_cull_squash`): `slope = q - 1` and `reach = slope * w`, where w is the rounding and combine support the bound already carries. A group takes its subtree's widest slope and reach, plus slope times its own support. The cull tests the bound widened by `slope * (band + pad) + reach`.
- `CullIndex::Entry` caches the squash. `CullIndex::plan` takes the batch's widest band and widens each squashed chain's scan region. `CullPlan::serves_band` refuses a region with a wider band than the plan was made for, when the document holds a squashed placement.
- The band is set where it is known: the C ABI refill paths, the volume bake, gradient-normal meshing, and `clay_eval_grid`, which takes it from the lattice's margin inside the host's region.

## Impact
- A document with no per-axis scale makes bit-identical cull decisions. The squash is exactly zero for a similarity, and a test pins it.
- A squashed document keeps a squashed item in the bricks its field reaches, and no others.
- No ABI, format or version change. `CullIndex::Entry` grows from 40 to 48 bytes.
- Mechanism A of #649, the chain-pad envelope below 75 contributors, is not addressed. #649 stays open for it.
