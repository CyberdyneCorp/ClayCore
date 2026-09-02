# Proposal: rejecting an item for a brick should not cost a cache miss

## Why

A `CullPlan` is ONE coarse cull over a batch's union region. Every brick in the
batch then culls the survivors AGAIN, against its own much smaller region --
that is the design, and it is why the plan is allowed to be coarse.

The second cull is the expensive one. On a 50,000-item document, a 24-brick dab:

```text
21,633  survivors of the batch cull
 9,634  of them survive any ONE brick's region
~12,000 rejected, per brick, 24 times over
```

And a reject reached the NODE to decide:

```cpp
const Node* n = e ? e->node : content.find(ids[at]);
if (!n || !n->visible) continue;
...
if (cull && item_influence_is_local(*n) && culled(geometry)) { ... }
```

`Entry::node` is a pointer into the layer's node map, walked in chain order, so
it is a cache miss -- spent to read `visible` and then to ask
`item_influence_is_local` about the node it landed on.

Both answers were already in hand. `Entry::local` IS
`item_influence_is_local` for that node, cached when the chain was built;
`Entry::bound` is the same bound; and a planned entry is visible by
construction, since `build_chain` skips the ones that are not. The reject was
re-deriving from a cache miss what the entry beside it already held.

Measured, 24-brick dab at 50,000 items, three interleaved runs:

| | per-brick compile | of which wasted test |
|---|---:|---:|
| before | 15.01 ms | 21.7% |
| after | 13.34 ms | 12.1% |

`BM_DeepDocCullPlanned10000` moves 1.36 -> 1.20 ms interleaved, with its
`instrs` counter -- the deterministic half -- unchanged at 10,504.

## What changes

The per-brick loop applies the survive test to the ENTRY, before it dereferences
anything:

```cpp
if (pruned && !(!e->local || e->bound.is_infinite() || e->bound.intersects(cull_test))) {
    cull_dropped = true;
    continue;
}
```

One expression covers both branches below it. An item survives unless it is
local, finite and misses the region -- the compiler's own test, inverted. A
group's entry is always local and its bound is infinite exactly when its subtree
is non-local, so the same expression reduces to the group's test. The two tests
below are then skipped for a planned chain, since this one has already decided.

`pruned` implies a cull region: both entry points drop the plan when there is
none (`c.plan = cull && index ? plan : nullptr`), which is what lets this read
`cull_test`. That is an invariant of a different function, so it is pinned by a
test rather than assumed.

## Impact

Nothing else moves. `CullPlan` is unchanged, so the plan's own scan --
`BM_CullPlanLocal{10000,50000}`, gated -- measures the same 0.006 and 0.026 ms
either way, and no memory is added anywhere.

The win is in the DENSITY direction, where a dab's region keeps a constant
fraction of the document and the per-brick compiles are 97% of its cull. That is
the direction `add-item-spatial-index` cannot help and `pack-the-cull-scan` did
not touch: a broad phase makes the plan cheaper, and the plan was already 3% of
the cost there.

## Rejected: carrying the packed boxes through the plan

`pack-the-cull-scan` gave each chain a box per entry with `!local` and an
infinite bound folded in, testable in six comparisons with no emptiness guards.
Carrying those through `CullPlan` for the per-brick cull to reuse was built and
measured, and it IS faster on this path -- 12.85 ms against 13.34 ms, waste down
to 6.0% -- but it was not kept:

- it costs the plan's scan 23%, because the hot loop gains a second store per
  survivor: `BM_CullPlanLocal50000` 0.0266 -> 0.0327 ms, on the row this project
  just gated;
- it costs 0.069 ms per plan at 50,000 items in the density direction, against
  the 0.55 ms it saves there;
- it adds a parallel array to every plan -- 0.5 MB on a 21,633-survivor one --
  and a second accessor that can drift from the first.

Reading the entry gets 73% of that win, costs nothing anywhere else, and touches
one file. The measurement is recorded here so the trade is not re-litigated
from first principles.
