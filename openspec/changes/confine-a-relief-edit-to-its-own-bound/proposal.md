## Why

Every stamp of an SDF relief or incise stroke but the last reported an
influence bound widened by 4 x strength world units on each side, whatever its
radius, and `clay_brick_cache_mark_dirty_nodes` dirtied by it (#672). The host
re-meshed the whole form on every relief stroke.

`clay_layer_apply_stroke` authors one node per stamp, so each stamp but the last
has the next stamp after it in the chain. The #650 walk (`node_reach_bound`)
dilates a node by the drag of the combines after it, taken from
`cull_pad_terms`, and `raise_blend_term` puts every extended op into
`blend_fixed = max(blend.support(), k)`. For a relief stamp `blend.k` is the
AMPLITUDE in world units (`brush/stroke.cpp` sets it to `templ.k * strength`),
so its quadratic support, 4k, was read as a blend support.

Measured on main (ca5883b9), the issue's stroke: unit sphere, radius 0.12,
strength 0.5, four samples x = 0..0.09, the host's template (unit sphere,
quadratic k = 1, rounding 1), default brick configuration:

| | earlier stamp's x width | last stamp's x width | bricks, `mark_dirty_nodes` over the stroke | one stamp |
|---|---:|---:|---:|---:|
| main, relief | 4.72 | 0.72 | 2,548 | 48 |
| main, incise | 4.72 | 0.72 | 2,548 | 48 |
| this change | 0.72 | 0.72 | 48 | 48 |

## What changes

- **A relief or incise ITEM's reach is its own bound.** Its combine is
  `a -/+ k * w(b)`, and `w` is exactly zero outside its falloff, which its own
  bound already holds -- so an edit to it leaves the running value bit-identical
  outside that bound, before and after, and every combine after it reads its
  operands at the same point. `node_reach_bound` takes neither the later
  siblings' drag nor an enclosing group's support for such a node. It still
  walks the ancestors: a hidden group hides it, and a non-local one keeps its
  own answer.
- **Unchanged:** a relief GROUP (it offsets by its children's combined value,
  which an edit to a child changes beyond the child's box), every node a relief
  item follows (the sphere a stroke is laid on still takes the stroke's drag),
  the cull pad, and every node whose op is not relief or incise.
- No ABI change. `clay.h` gains a note beside the #650 one on
  `clay_layer_node_influence_bound`; `docs/05` a section.

## What building it found

- **The planned fix was a narrower drag, and the drag was not the question.**
  The plan was to give the walk its own per-node term -- a relief carries a
  changed running value by `|k|`, not `4k` -- and to clip the dilated box to
  the union of the later siblings' influence bounds. Both answer "how far do the
  stamps AFTER a node carry its edit", which is the right question for a node
  whose edit changes the running value outside its box. A relief stamp's edit
  does not: there is nothing outside its bound for the later stamps to carry,
  so the exact answer is its own bound, with no term at all.
- **The `|k|` term would have been less conservative than what it replaced.**
  Folded as a MAXIMUM, as the chain terms are, `k` covers one relief, while
  overlapping relief stamps SUM where their falloffs meet (each subtracts up to
  its own `k` at the same point) -- the accumulation #666 measured for smooth
  chains. `4k` happens to cover four. Not measured here; the term for nodes
  BEFORE a relief stroke is left exactly as it was.
- **The clip needs care twice.** A box intersection does not commute with the
  band a consumer adds later; the band-agnostic form is a clamp into the
  dilated box, the shape `head_within` (#639) already uses. And holding the
  later siblings' bounds in `ChainDragMemo` makes the memo stale across the
  deformer edits an undo step currently keeps it across. Neither is needed for
  this issue, so neither landed.
- **The first oracle could not see a 0.1 shrink.** At 0.05 voxels (0.4 bricks)
  a reach shrunk by 0.1 left no stale brick; shrunk by 0.3 it left stale bricks
  in 23 edits. At 0.025 voxels the 0.1 shrink leaves 1-6 stale bricks in each
  of 20 edits, and the oracle runs in 3.5 s.
