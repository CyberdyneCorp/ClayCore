# Design

## The walk that replaces `root_ancestor`

`node_command_bound` (`src/scene/commands.cpp:317`) does two things today: it
finds the root, and it takes that root's bound. Only the first half is worth
keeping, and only as a side effect.

```
bound = node_influence_bound(content, node, layer)      // the node itself
cur   = node
loop:
    parent = locate(cur).parent
    if parent == kNoNode: stop
    if !op_is_local(parent.op): return Aabb::infinite()
    bound = bound.dilated(group_support(parent, layer))
    cur = parent
```

`group_support` is the expression `node_influence_bound` already uses for a
group (`src/scene/bounds.cpp:1014`): `ccombine_extended_support(op, k, rounding
* layer.xform.scale)` for an extended op, `cmax(blend.support(), blend.k)`
otherwise. It is lifted out of that function rather than re-derived, so the two
cannot drift about what a group's blend reaches.

The walk is bounded by the node count for the same reason `root_ancestor` is:
`roots` is a public vector and the walk must terminate whatever a caller wrote
there.

## Why the support is the whole correction

A group's value is a combine over its children. Changing child *c* changes the
group's value only where *c*'s own value is inside the combine's support of the
running accumulator — that is what a blend's support *means*, and it is
already the dilation `node_influence_bound` applies to the union of the
children. Applying it to one child instead of to the union is the same
inequality with a smaller left-hand side.

Nesting composes: the inner group's output is dilated by the inner support, and
that dilated box is what the outer group sees, so the outer support dilates it
again. Sum of supports along the path, one dilation per level.

## Where it is NOT enough, and why that is already handled

A later sibling in the same chain whose op is non-local — an intersect — reads
the running accumulator and can move the result arbitrarily far. That is not a
new hole: with items at the layer root, `root_ancestor(node) == node`, so the
root-ancestor rule never covered it either. It is covered where it has always
been covered — `item_influence_is_local` makes such an item's own bound
infinite, and `node_influence_bound` propagates infinite up through any group
holding it, which this walk preserves by checking `op_is_local` at every level.

## The two bounds already disagree, and only one of them is loose

A host asking where an edit landed is answered by
`clay_brick_cache_mark_dirty_nodes` / `clay_layer_node_influence_bound`, which
use `node_influence_bound_in_document` — the node's OWN bound, unioned over
instancing layers (`bindings/c/clay_c.cpp:10946`). The engine's own seed drop
uses `node_command_bound`, which is the root ancestor's.

So the two paths already give different answers for the same edit, and the
tight one is the one already shipped to hosts. Measured through the device
harness on a grouped 1000-item document: a drag frame dirties 66.9 bricks
whether or not the document is grouped — the host's region is identical — and
the grouped frame is several times slower, because every one of those bricks
lost its seed to the wider internal bound and took a full walk.

That is worth stating for two reasons. It is evidence that the tight bound is
sound in practice rather than only in argument, since it is what the ABI has
been handing hosts. And it means this change makes the two agree, which is one
fewer thing that can be true in two ways.

## What this does for the frontier

`command_frontier` (`bindings/c/clay_c.cpp:2946`) resolves the root ordinal an
edit dirties from, and `touch_region_from` keeps in-bound seeds whose prefix can
still serve that ordinal. The ordinal is unchanged by this work — an edit inside
root *k* still dirties from *k*. What changes is that the region it applies to
no longer covers the whole root, so seeds outside the edited child's reach are
never visited at all.

That is why the measured win is larger than the box shrinks: a brick outside the
region keeps its value *and* skips the per-brick culled tape compile, which at
1000 items is the 43 µs the refill actually spends per brick.

## Rejected

**Caching the ancestor path per node.** The walk is O(depth) with no allocation
and runs twice per command. A cache would need invalidating on every reparent,
which is a correctness surface bought for a term that does not show up.

**Reporting the child's bound alone and letting the caller dilate.** Two callers
would then have to know the group rule, and one of them is a host across the
ABI. The bound is the answer; the dilation is part of it.
