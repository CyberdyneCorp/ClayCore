## Context

`clay_document_undo_bound` / `_redo_bound` report, per command in the step,
`command_influence_bound` taken before and after the replay, unioned
(`UndoStack::replay`, `src/scene/commands.cpp`). For a `SetDeformersCmd` that
target is the NODE: `node_command_bound` -> `node_influence_bound_in_document`.
A Move segment is exactly that command -- `brush::moved_chain` puts one grab per
drag image at the HEAD of every chain the drag reached -- so undoing it reports
the node's whole bound. The node's bound also grows with the chain:
`deformed_local_bounds` dilates the whole local box by every grab's pull.

## D1. The argument: a head of exact-identity links changes nothing outside its balls

The chain runs `p -> d0 -> d1 -> ... -> prim` (`ctape_prim_local`), accumulating
`offset += deform_offset(link, wp)` and `wp = deform_point(link, wp)` per link.

Take the two chains a step goes between. Strip their longest common TAIL (links
compared bit for bit; a payload the comparison does not read makes them unequal,
which is the safe direction). What is left is a HEAD on each side. Suppose every
link in both heads is the identity outside its own ball -- returns `wp`
untouched (a warp) or adds exactly `0.0f` (an offset) wherever its weight is
zero. Then for a point `p` outside every one of those balls, link 0 hands `p` on
unchanged, so link 1 sees `p` and hands it on unchanged, and so on: both heads
deliver the same point and the same offset (`x + 0.0f == x`) to the same tail.
The item's raw field is BIT-IDENTICAL outside the union of the balls.

Every combine above the item -- its own op, a group's, the layer mirror's seam,
a layer's composition -- is a function of its operands at `p`. So the document's
raw field is bit-identical outside the union of the balls' images too. This is a
stronger statement than the band-clamped one every other reach in `bounds.cpp`
makes, and it is what `test_deformer_head_reach.cpp` checks, bit for bit, on the
reference evaluator.

A twist (or any whole-item link) in either HEAD breaks it: it moves the point
before a later link's ball is tested, so the region is that link's PREIMAGE, not
its ball. A whole-item link in the common TAIL costs nothing -- it sees the same
point on both sides. So a grab added at the FRONT of a twisted chain narrows, and
one appended BEHIND the twist does not.

## D2. Which deformers qualify, and why these four

The candidates are `link_support`'s finite set -- grab, magnify, radial pose,
blob, alpha -- all gated by `cregion_weight`. Finite weight is necessary and NOT
sufficient; what the argument needs is that the KERNEL returns its input
untouched where the weight is zero.

| kind | kernel past the rim | qualifies |
|---|---|---|
| grab | `if (w == 0.0f) return p;` | yes |
| magnify | `if (w == 0.0f) return p;` | yes |
| blob (offset) | `if (w == 0.0f) return 0.0f;` | yes |
| alpha (offset) | `if (weight <= 0.0f) return 0.0f;` | yes |
| radial pose | no early-out: `centre + rotate(p - centre, -angle * 0)` | **no** |

Radial pose was on the list in the first draft of this design. The raw check
refuted it: `centre + (p - centre)` is not `p` in float, so the field moves by an
ulp wherever the item is -- 1,917 of 68,796 lattice points outside the ball moved
on the test fixture. An ulp is enough to flip a stored fp16 brick. Pose keeps the
node's bound; `test_deformer_head_reach.cpp` pins the reason, so the day pose
gains an early-out the test says it may join.

THE EASING, second condition. The weight at the rim is `cease(ease, 0)`.
`ease_out_sine` computes `cos(pi/2)` there, which is not zero in float, and
`bounds.cpp` already records a curve that returns 5.96e-08 at its zero end: a
grab under such a curve is a rigid translation by a hair EVERYWHERE, and has no
support at all. And the host computing zero is not enough -- a backend with fast
math or fused multiply-add may not. So `ease_is_zero_at_rim` refuses the
families whose rim runs through a transcendental or a multi-term polynomial (the
sines, `out_expo`, the circs, `in_bounce`, `in_out_bounce`) whatever the host
computes, and accepts the rest only when the host computes exactly zero. The
accepted ones reach the rim through a product with `t = 0`, a guarded branch, or
`1 - 1*1*...*1`, which every IEEE backend evaluates exactly.

## D3. The dilation is shared, not restated

The balls are in the item's chain frame. They are placed by the functions that
place the item's own geometry, taken rather than re-spelled:

- **Placement** -- `placed_local_bound`, which is the body `geometry_bound` had,
  with the local box made a parameter. `geometry_bound` now calls it with
  `item_local_bounds`; the head reach calls it with the balls. One body, so the
  item transform, the per-axis scale, EVERY mirror and radial copy, the seam
  supports, the rounding and the item's own combine support cannot drift apart
  between "where the item is" and "where its head can change it".
- **Repetition** -- `repeated_local_bounds` on the balls, because repetition
  folds the point before the chain sees it. An infinite grid is refused.
- **Groups** -- `dilate_by_ancestors`, the walk `item_geometry_reach_in_document`
  already uses: node_reach_bound's per-group support with its #515 predicate,
  refusing a morph or hidden group.
- **Folds** -- `layer_reach_in_document`, i.e. `folds_from_layer_support`, the
  one definition every other reach goes through.
- **Instancing** -- the union over every layer sharing the content, exactly as
  `node_influence_bound_in_document` takes it.

None of these dilations is what makes the answer sound -- D1 is exact -- but
they are the ones every other reach takes, and taking them keeps this answer
inside the band-clamped framework the rest of `bounds.cpp` is written in rather
than beside it. They cost a blend radius or two per side. The mutation checks
show the fold and the displaced end are pinned by the box test; the mirror copy
is pinned by the brick oracle.

## D4. The displaced end is margin, kept on purpose

D1 needs the ball at the grab's centre only: the kernel reads the weight at the
sample point. The ball at `centre + pull` is margin. It stays, because clay.h
has always promised a move's two ends, and because the live Move reports the
same segment as its ball dilated by the pull (`move_surface_impl`): an undo that
reported LESS than the gesture that made the edit is a surprise no host could
debug. It costs the pull's length along one axis.

## D5. Intersected with the node's bound, never substituted

`apply_bounded` still takes `command_influence_bound` on both sides and then
CLIPS it to the head's box. Two consequences, both wanted:

- The undo bound is never larger than the node's -- the acceptance's "no larger
  than the node's bound" -- even for a ball that pokes out of the node's box
  (the part outside is where the node's field is beyond the band on both sides,
  which is the contract every influence bound already makes).
- Every refusal falls back to exactly the old answer: nullopt means no clip.

## D6. Mixed steps

The narrowing is per COMMAND, inside `UndoStack::replay`. A step holding a
grab and a transform of another node reports the grab's clipped box unioned
with the transform's two ends. A Move commit that also ran the complexity
policy -- a consolidation inside the same step -- reports that consolidation's
own bound beside the grabs'. Tested: `a step that also carries another command
still covers that command`.

## D7. Where it is NOT applied

`apply_edit` -- the forward path of `clay_layer_add_deformer` and of a Move
commit -- is unchanged. The live Move states its own reach (`move_surface_impl`),
and a host adding one deformer by hand dirties with `mark_dirty_nodes`, which is
the node's bound by contract. The rule is one function
(`command_head_delta_bound`), so applying it forward later is one call site.

## D8. The oracle had to be taken on a COPY of the document

The first brick oracle built the "fresh" cache on the same document after the
undo, the shape `test_intersect_delta_oracle.cpp` uses. With the mirror image
deliberately dropped from the bound, it reported ZERO stale bricks. The seed
store belongs to the document (clay.h, `clay_resume_stats`), and the refill
resumed from the very seeds the too-narrow bound had failed to drop, reproducing
the stale values and agreeing with them. The oracle here rebuilds from a
`clay_document_save_memory` / `clay_document_load_memory` copy, which has no seeds; with that,
the same mutation reads 8 stale bricks. `test_intersect_delta_oracle.cpp` has
the same shape and is not changed here -- it is noted as a gate that may not be
able to fire on its own failure mode.

## D9. What this does NOT fix

The per-brick price. The cost of one refilled brick still grows with the
chain's length -- 7.4 us at 1 grab, 28.1 us at 160 on the probe's fixture,
with none of those grabs reaching the refilled bricks -- and where that cost
goes is not measured here. This change removes the node-extent factor only
(numbers in proposal.md). The issue's
second route -- a refill over a baked volume costing close to an analytic
item's (~430 us vs ~7 us a brick, measured by the host) -- is a separate problem
and is not attempted.
