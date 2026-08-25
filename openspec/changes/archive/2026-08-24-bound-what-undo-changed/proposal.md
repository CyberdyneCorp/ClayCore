# Proposal: say what an undo changed, so a host can dirty that and nothing more

## Why

`clay_document_undo` reports one thing: whether anything was undone. A host
that keeps a brick cache has to turn that into a region to dirty, and the
narrowest region it can honestly name is the whole layer — so every undo pays
a full fill.

Reported in #210, measured on a model of 1043 surface bricks:

| | keys refilled and meshed | refill | mesh |
|---|---|---|---|
| a dab | 27 | 0.9 ms | 4.0 ms |
| undoing that dab | 2940 | 66 ms | 141 ms |

207 ms to reverse a 5 ms edit, and it grows with the model rather than with the
edit. The dab bounded itself with `clay_brick_cache_mark_dirty_nodes` and got
27 keys. The undo of that same dab cannot express a bound narrower than
`clay_brick_cache_mark_dirty_layer` — "the union over a layer, what a first,
full fill marks".

**The host cannot work it out, and the ways it might try are worse than the
slow undo.** Diffing the layer's nodes across the call catches adds and removes
and misses an in-place change: an undone move, resize or colour edit keeps its
node id, so the diff sees nothing and the host under-dirties. That is the
failure the header warns about — "a bound that is too tight leaves visibly
stale bricks at a blend seam" — and it is silent and on screen. Capturing every
node's bound before and after covers moves and resizes, still misses a colour
or a blend edit, and costs O(nodes) queries per undo.

The engine holds the exact list of commands it applied. Nothing else does.

## What changes

- **A bound comes back with the undo.** `clay_document_undo_bound` and
  `clay_document_redo_bound` do exactly what `clay_document_undo` and
  `clay_document_redo` do, and additionally report the world-space INFLUENCE
  bound of what they applied, in the three-state shape
  `clay_layer_node_influence_bound` already uses (nothing / a finite box /
  unbounded). It feeds straight into `clay_brick_cache_mark_dirty`, whose
  both-NULL spelling is the unbounded case.
- The existing `clay_document_undo` / `clay_document_redo` keep their
  signatures and their behaviour. A host that does not keep a cache does not
  have to learn a new call.
- The bound is the union, over every command in the step, of what that command
  targets BEFORE it is applied and AFTER — which is what makes it cover a move
  (both ends), a removal (the node that is gone), and an add (the node that was
  not there), none of which a single side can see.
- It is deliberately CONSERVATIVE where being tight would cost correctness: a
  node inside a group reports its root ancestor's bound, because a group's
  blend spreads a child's influence past the child's own box; and an edit to
  content shared by instanced layers reports the union over every layer that
  shares it.

## What this does not change

- No existing entry point changes signature, and no `clay_result` value is
  added — the outcome crosses as an out-parameter, as the c-abi spec requires.
- Undo stays opt-in per document, and the bound is computed only for the step
  actually applied, so a document with undo off pays nothing.
- Nothing here dirties a cache. The cache still learns about an edit only when
  the host tells it, which is the one path `brick-cache` documents.

## Impact

- `bindings/c/clay.h`, `bindings/c/clay_c.cpp` — two new entry points.
- `include/clay/scene/commands.h`, `src/scene/commands.cpp` — `UndoStack::undo`
  and `redo` gain an optional out-bound; one new function computes what a
  command targets.
- ABI 0.40.0. No format change: `kSceneMinor` is untouched, because no command
  and no document field changed.
