# Design

## The two questions

```
GENERIC INFLUENCE                       EDIT DELTA
"where can this node change             "where can THIS EDIT have moved
 the field?"                             the surface?"
        |                                       |
        +-- Intersect -> the layer              +-- a supported move -> the swept
            (item_nonlocality,                      old/new support
             unchanged)                          +-- everything else -> nullopt,
                                                     and the caller keeps the
                                                     conservative union
```

Both are computed on every edit, and the second is used only when both sides of
the apply produce one. Keeping the first is not waste: it is what a refusal
falls back to, and a refusal must never cost correctness to discover.

## Why the swept union is enough, and where each term comes from

Let `G_old` and `G_new` be the operand's geometry bounds on the two sides —
`item_geometry_bound`, so already dilated by rounding, by the item's own combine
support, and by every copy the layer's mirror and radial modes emit, with their
seam blends.

1. **The band-clamped claim.** At a point `p` outside `G_old ∪ G_new` dilated by
   the band, the operand's own field is `> band` on both sides. The chain's
   value after the operand is `max(acc, item) >= item > band` on both sides, so
   the value a brick STORES — band-clamped and quantized — is the same on both
   sides. That is the whole argument, and it is why the claim is about the
   surface and the band around it rather than about the field.

2. **Why the band is not a term in the box.** Every consumer dilates by it
   already: `BrickCache::mark_dirty` dilates the region by the band, and the
   seed store dilates each brick by band + pad. Adding it here would double it.

3. **Why the chain pad IS a term.** The raw value out there is NOT unchanged —
   it is the moved operand's own distance. A local op does not have this
   problem: `min(acc, big)` is `acc` bit for bit outside the support, so nothing
   downstream can see the edit at all. Here a smooth combine further down the
   chain, whose other operand is within its support of the running value, can
   carry a beyond-band difference back toward the band. `blend_cull_pad`
   measured exactly that drag for culling (0.26 below the base sphere's own
   distance at k = 0.06 over 600 dabs) and `cull_pad` is its expression,
   resolved against the same effective contributor count. This reuses it rather
   than deriving a second one — bounds.cpp warns in three places that two
   spellings of "how far a combine reaches" will diverge.

4. **The ancestor groups.** `node_reach_bound`'s walk, term for term, through
   `group_blend_support`. The one difference is that it does not take
   `node_reach_bound`'s layer-extent escape for a non-local group: a group only
   has to combine POINTWISE here, and an intersecting group leaves the running
   value beyond the band on both sides for the reason the item does.

5. **The layer folds.** `layer_reach_in_document`, which is
   `folds_from_layer_support` — the same sum the influence path carries.
   Removing it produced 203 sign changes outside the box on the
   layer-composition fixtures, worst 0.529.

6. **Instancing.** The union over every layer sharing the content, exactly as
   `node_influence_bound_in_document` takes it (#325). Shared content is not
   ambiguity here — each layer contributes its own swept box under its own
   transform.

## The proof domain, as fallback rules

`command_surface_delta_bound` returns `std::nullopt` — and the caller keeps the
conservative union — for:

| refused | why |
|---|---|
| any command but `SetTransformCmd` | a different question; a prim, op or blend change moves the operand's field where it stands |
| an op that is not `Intersect` | nothing to narrow: the influence bound is already this box |
| a node absent, hidden, or a group on either side | there is no operand to sweep |
| a deformer chain on the operand | a warped field underestimates distance by its Lipschitz factor, and the bound carries no dilation for that |
| a non-uniform per-axis scale on the operand or on a layer holding it | the same underestimate, by exactly `max(s)/min(s)`: `cscale_nu_dist` multiplies the local distance by the SMALLEST component, so the field outside the box is short of the distance the box was drawn against and `> band` holds only out to `band * max(s)/min(s)`. The box itself is right — `item_geometry_bound` composes `scale_matrix` — which is what makes this a field refusal and not a geometry one. `placement.h` excludes a squashed layer from the sibling classifier for the same mechanism |
| a sampled-volume primitive | its field outside the samples it stores is whatever the extrapolation says |
| an unbounded primitive, an infinite grid repeat | no finite geometry to sweep |
| a GATE on the operand | a gated combine is `mix(acc, combine(acc, item), mask)`, and a lerp of a beyond-band value is not beyond band |
| a gate or a spatial morph ANYWHERE in the layer's chain | the same lerp, downstream: it carries the beyond-band difference into the band at any distance |
| a morph in a layer fold at or above the layer | the same, one level up |
| an infinite or non-finite support anywhere, an empty or infinite box | nothing to claim |

**The squash rule was found by review, and the fixture that should have caught
it certified the opposite.** "squashed per axis" asserted the bound was PROVABLE
for `scale_axes = (2.2, 0.5, 1.4)` and could not see the defect: where that
operand's field is short (between 0.15 and 0.66 of its box) the body's own
accumulated value already dominates the `max`, so the intersect never returns
the short value there. The fixture now asserts the refusal, and two new ones
put the short value where the max DOES return it — a disc-shaped operand
(`min(s) = 0.1`) inside a body big enough that `max(acc, item)` is the item —
so that the revert has somewhere to fail.

The gate and morph rules are the ones a reader is most likely to think
unnecessary. They are the cases where the difference between "band-clamped
equal" and "equal" becomes visible, and a local op never meets them because for
a local op the difference is zero.

## Gating a term that can only widen the box

Three of the four terms are DILATIONS. Every numeric gate on the box is
one-sided in the shrinking direction — `delta_growth < 4.0`, the two count
comparisons, `volume(delta) < 0.35 * volume(conservative)` — so a term that only
widens it can be deleted and every one of them passes more comfortably. The fold
term escaped that because a fixture existed where its absence changed the FIELD;
the chain pad and the ancestor group supports did not, and were shipped
ungated until review said so.

**The chain pad now has a fixture, because it is a field claim.** A body, a hard
intersect operand, and one smooth dab at k = 0.25 whose surface passes 0.03 from
a point where the running value IS the operand's own distance — 0.30 before the
move, 0.95 after. `smin` turns that into −0.103 against +0.028: a sign change
0.30 from the operand's box, twice the band, and `cull_pad` = min(4k, 2.80k) =
0.70 is what covers it. Dropping the term: 288 sign changes, 38 samples entering
the band and 560 leaving it, worst |db| 0.244.

**The ancestor supports cannot have one, and this is why.** A group's blend
drags a beyond-band value by strictly less than its own support, and `cull_pad`
already carries 2.80k of that support's 4k — every node feeds it, groups
included. So the window a probe would have to find a counterexample in is the
1.2k between them, where the correction is under a hundredth of the band and
below the fp16 the brick cache quantizes to. The term is conservative and the
walk it mirrors (`node_reach_bound`) has its own tests; what this branch owed
was a gate that it is PRESENT. That is an arithmetic pin: five nodes, a dab at
k = 0.4 and a group at k = 0.25, so `cull_pad` = 1.12 and the group support =
1.0, and the six faces of the reported box asserted against 0.3 ∓ 2.12 by hand.
Deleting the pad reports −0.70 where −1.82 is derived; deleting the ancestor
walk reports −0.82. The two helper values are asserted beside it, so a change to
either formula fails at the formula and says which one moved.

## Two instruments, and they do not see the same things

The PROBE samples the field and asks whether a point's meshing classification
changed outside the claimed box. The ORACLE keeps a brick cache across the edit,
dirties the reported region, refills, and compares every stored brick and the
mesh against a cache rebuilt from nothing.

They are not redundant, and the fold term shows why: the probe failed on it
immediately, while the first oracle fixture could not see it at all — a fold
only moves the document's surface where the two layers' fields are within its
support of each other, and a small shape folded beside the form leaves that
region empty of bricks. The oracle's fold fixture is now a box over the whole
form, and the term's absence costs 93 stale bricks.

The lesson is the fixture's, not the test's: a gate that passes because its
fixture has nothing to disagree about is the failure mode this repository keeps
meeting, and the way to find it is to break the code on purpose and check the
gate notices.

## What was tried and rejected

**Weakening `item_nonlocality` for Intersect.** The one-line change. It makes
the public influence query, per-brick culling (which may never drop an
intersect), the generic dirty-node API and every other Intersect edit unsound.
The measurements that put the layer bound there are in bounds.cpp and are not in
dispute.

**Answering the delta from one side of the apply.** The after box alone is what
a naive implementation produces, and the probe's own self-test asserts it FAILS
— sign changes and band changes outside it — so the sweep is not optional and
the probe can see when a bound is one term short.

**Narrowing `clay_brick_cache_mark_dirty_nodes` instead of adding an entry
point.** That call answers "an edit to this node" with no before state; it
cannot know a move happened, and making it assume one would be wrong for every
other edit a host makes through it.

**A per-brick predicate inside the swept box.** The region was the dominant
cost — 9,680 bricks against 286 at ten times the extent — so it comes first.
Whether the 286 can be narrowed further is a separate measurement.

## Cost

Two extra `item_geometry_bound` calls, two `cull_pad` walks of the layer's node
map and two ancestor walks per edit — measured at 0.002 ms a frame beside the
influence bound that is still taken. `cull_pad` walks the node map reading blend
parameters only; it is not the geometry walk #451 was about.
