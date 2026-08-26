# Proposal: an edit's box must cover every place the node is

## Why

`clay_layer_node_influence_bound` promises "the box outside which this item
cannot change the field", and `clay_brick_cache_mark_dirty_nodes` dirties by
that box. On a document with an instanced layer the promise is false by most of
a band:

```
worst band-clamped drift outside the declared box
  gnarly corpus                          0.119248     <- band is 0.15
  a two-layer instance, isolated         0.103183
  the same document without the instance 0
```

`instance_layer` copies the `Layer` and SHARES the `SdfContent` by
`shared_ptr`, so one node is compiled once per instancing layer, each under
that layer's own transform. Editing it moves every copy. Both entry points
answered for the single layer they were handed, so a host dirtied one copy and
left the others holding values up to 0.103 wrong — stale geometry, with nothing
to say so.

**The union already existed.** `scene::node_command_bound` walks every layer
sharing the content and unions, which is why the UNDO path was right and the
two queries a host drives were not.

## What Changes

`scene::node_influence_bound_in_document` is that union, shared so the query,
the dirty call and the command path cannot disagree about where an edit
reaches. Both ABI entry points use it.

The per-layer `node_influence_bound` is unchanged and still answers the
narrower question, which the compiler and the cull index want.

## Impact

**A behaviour change with no ABI change.** No symbol moves, no signature
changes, nothing recompiles. Two calls report a LARGER box than before on a
document with instanced layers, and the same box on every document without one
— which is why no existing test moved.

A host dirtying by the new box refills more bricks on such a document. That is
the cost of the answer being right; the previous figure was cheaper because it
was wrong.

## Non-goals

**The cull path.** `item_influence_is_local` and the per-brick cull are
untouched: culling reads `item_geometry_bound` per layer and compiles a tape per
layer, so it never had this defect.

**Tightening a non-local op's bound**, which is issue #319 and needs a fixture
that can tell a valid bound from a too-small one first (#326).
