# Proposal: expose scene groups to hosts

## Why

This one is smaller than it first looks, and the correction is the point.

Building `examples/35_hard_surface_helmet.py`, the operation a hard-surface
model is made of — *a plate is a shell INTERSECTED with a cutter, and that
result is then unioned into the helmet* — appeared to be inexpressible. An op
applies against the whole accumulated field, so an intersect meant for one
panel intersects everything already there. The example works around it by
giving every plate its own layer, because a subtraction inside a layer trims
only that layer's field.

The workaround is sound and the example documents it as a technique. But the
premise was wrong: **the engine already has groups.** `scene::Node` carries
`is_group`, `tape_build.cpp` has `compile_group`, and `Op::None` exists
specifically so a group's children apply inline to the outer chain. Sub-trees
that compile as a unit are a solved problem in the scene model.

They are simply not exposed. The only "group" in either binding is
`clay_document_begin_undo_group` / `end_undo_group`, which is undo bracketing
and unrelated. Searching both binding surfaces for scene groups returns
nothing.

So the gap is a binding-surface gap, not an engine one, and this change is
plumbing rather than design.

## What it buys

`(A ∩ B) ∪ C` becomes sayable from a host. Concretely, for the helmet: a plate
becomes one group — shell, intersect cutter — combined into the assembly with
its own op and blend, instead of a whole layer per panel. Layers go back to
meaning what they mean everywhere else (a thing you show, hide, lock and
transform) rather than doubling as an expression-grouping mechanism.

It also removes a scaling problem the example runs into: nine plates means
nine layers, and every one of them has to be framed, ordered and reasoned
about separately.

## Approach

Expose what the scene model already does:

- create a group node in a layer, with an op and a blend like any other node
- add children to it, including nested groups
- `Op::None` for a group whose children apply inline to the outer chain, which
  is the existing semantics

Bounds, culling and undo already handle groups — `node_influence_bound` dilates
a group's bound by its own blend support, and `compile_group` already refuses
to emit a carving group with nothing beneath it. The work is the ABI surface,
the binding, the round trip, and the tests that pin the semantics from outside.

## Open questions

- Whether a group can be re-parented after creation, or is built once. The
  command vocabulary has to express whichever is chosen, with an exact inverse.
- Whether a group's transform composes with its children's, and in which order.
  The scene model may already answer this; the proposal must state it rather
  than leave it to be discovered.
- Whether the C ABI hands back a node id per group (consistent with every other
  node) and how a host enumerates a group's children.

## Impact

`c-abi` and `python-bindings` gain the group surface. `scene-model` gains
requirements that pin behaviour already implemented but never specified from
the outside. `file-io` should need nothing — groups already serialise, and a
round-trip test will confirm it rather than assume it. Purely additive.
