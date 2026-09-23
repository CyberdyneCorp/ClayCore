## Why

`benchmarks/undo_bound_oracle_probe` under `RV_RAWBOUND`, seed 5128 in rich
mode, found the raw field moving by an ulp at samples outside the undo bound
dilated by the band, identically on main before and after #648 (#650). The
issue suspected the group blend's support. It is not the group: the group in
that seed is the first root, so its combine never applies (#515).

The node is the FIRST item of its chain, and the item after it blends smooth at
k = 0.297. The first item of a chain has no combine -- it IS the running value
-- and any item is the running value wherever it is the nearest thing, so an
edit to it changes that value far from its own box. Beyond the band a hard
union leaves that alone (`min()` is exact). A smooth combine further down does
not: `csmin(a, b)` reads `a` wherever `|a - b| < support`, so with `b` in the
band it reads `a` out to band + support and carries the change back into the
band. In the seed, a grab's eased rim changes the node's value by an ulp inside
the grab's ball, 0.3 from the node's surface; the sibling's blend lifts that
into the band, and the undo bound -- the ball clamped into the node's bound --
stopped at the node's box.

It is not an ulp in general. Measured on main (8b7a5713), two r = 0.3 spheres
0.3 apart, the second smooth at k = 0.3, the first moved 0.1:

| | band samples moved outside box + band | worst | stale bricks, `mark_dirty_nodes` |
|---|---:|---:|---:|
| main, at the root | 6,359 | 0.0438 | 25 |
| main, in a blended group | 6,359 | 0.0438 | 25 |
| main, second at k = 0.1 | | | 8 |
| this change | 0 | 0 | 0 |

## What changes

- **`node_reach_bound` dilates by the drag of the combines after the node**, at
  each level, before the enclosing group's support: `cull_pad_terms` raised
  over the later siblings in that chain, resolved at each profile's full
  support. A later sibling group contributes its own combine only. That makes
  `clay_layer_node_influence_bound`, `clay_brick_cache_mark_dirty_nodes`, the
  command path and the undo bound (through the #639 clamp) cover the fillet.
- **Nothing widens for a node with only hard combines after it** -- every node
  appended last, so a stroke's dabs, and every document without a smooth blend
  report bit-identical bounds.
- **`ChainDragMemo`**, per-chain suffix maxima filled from the end, owned per
  query by `LayerExtent`, threaded through the drag frontier's loop and shared
  across an undo step's replay.
- No ABI change; `clay.h` gains a note on `clay_layer_node_influence_bound`.

## What building it found

- **The envelope is the wrong resolution.** Resolving the later siblings' terms
  at the chain envelope the cull uses (`blend_total`) left 110 band samples
  moved by up to 0.0123 on the two-sphere fixture: the envelope is a k-multiple
  fitted to what a CULL may drop against an fp16 tolerance, and a single blend
  provably reaches its whole support. Full support: 0.
- **The #639 head narrowing needs nothing.** The first cut also dilated the
  head's ball, and the oracle's refills rose 72% / 56%. A link that is the
  identity outside its ball leaves the raw field bit-identical there and every
  combine is pointwise, so the ball needs no drag term; only the clamp into the
  node's bound was too tight. Reverted: refills rise 2.5% / 2.8%.
- **The walk is over LATER siblings, so loops paid it per node.** A drag
  frontier resolving 1,428 warps of a 10,000-item smooth layer: 0.39 -> 65 ms.
  Undoing a 2,134-warp Move over the same count: 0.85 -> 94.6 ms. With the
  memo: 0.45 ms and 1.04 ms. The replay's memo is cleared after any command but
  a deformer or colour edit; a test holds the clear, and fails without it.
- **The frontier's mirrored ridge resumes MORE.** The drag frontier prepares
  prefix seeds over each dragged node's reach; under the mirror the wider reach
  now spans the four corner bricks, so they resume (16 / 0) where the
  unmirrored control still refuses them (12 / 4). The bit-exact parity check
  against a fresh oracle holds every frame, so the test's `==` became `>=`.

## Measured on the oracle (400 trials each, `RV_RAWBOUND`)

| | raw-bound violations | stale trials | bricks refilled |
|---|---:|---:|---:|
| plain, main | 0 | 0 | 1,397,332 |
| plain, this change | 0 | 0 | 1,432,408 (+2.5%) |
| rich, main | 10 (seed 5128) | 3 | 2,394,500 |
| rich, this change | 0 | 3 | 2,461,088 (+2.8%) |

The three rich stale trials (5111, 5128, 5229) are the brick build disagreeing
with the raw field, #649's, and identical on main. `RV_FORWARD` reports 12 stale
trials on main and on this change alike.

## What it does not cover

A GATED node after the edited one mixes the running value with its own combine,
and a lerp of two beyond-band values is not beyond band, so it can carry the
difference in at any distance. `chain_carries_only_supports` already refuses it
for the intersect surface delta; this term does not model it.
