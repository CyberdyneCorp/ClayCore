# Proposal: read a placed node back

## Why

Issue #317. Four calls write a placed node's state —
`clay_layer_set_transform`, `clay_layer_set_prim`, `clay_layer_set_color`
and `clay_layer_set_op_blend` — and the only accessor on a node is
`clay_layer_node_prim`, which answers *which primitive* and nothing else.

That is enough to reload a document and FIND an item, which is exactly what
#91 and #99 built it for: ask what the node is, then call the typed reader
that applies. The typed readers exist for an armature
(`clay_layer_armature_parents`, `_signs`) and for a stroke's points
(`clay_layer_stroke_points`). **There is none for a plain item**, so the
question "where does this sphere stand, how big is it, how does it combine"
has no answer in this ABI.

A host that lets an artist place a primitive, move it with a manipulator and
change its operation afterwards therefore keeps its own table of those values
and **saves it in a second file beside the `.clay`**, keyed by node id. A
document opened without that side-car shows a box and can say nothing else
about it — not its size, not its operation, not even that it was placed rather
than deposited by a stroke.

Two things had to hold before that side-car was safe, and both do:

- a node id survives a save and a reopen, including across the gap a removal
  leaves, so the table's key still means the same node;
- `clay_layer_node_influence_bound` is a partial answer and is used as one,
  but it is NOT a position. It is dilated by rounding and blend support, and
  under a layer mirror it covers the reflection too — an object placed at
  x=0.9 in a mirrored layer reports a bound centred on the origin.

The copy is the cost. Keeping it correct across undo, redo and reload is the
host's problem, and the host solves it by following the engine's history by
depth — machinery that exists only because the engine will not answer.

## What it buys

The side-car comes out. A host reads placement, size and operation off the
document it just opened, and one source of truth survives undo and redo
because it IS the document.

It also completes the pairing #91 documented. Enumerate a layer's nodes, ask
`clay_layer_node_prim` what each one is, then call the reader that applies —
`clay_layer_stroke_points` for a curve, `clay_layer_armature_parents` for a
rig, and, with this change, three calls for the item every other branch of
that walk falls through to.

## Approach

Three readers, symmetric with the setters they mirror and on the same terms as
the rest of the reading surface:

- `clay_layer_node_transform` — position, rotation axis/angle and scale, the
  four arguments `clay_layer_set_transform` takes, so what comes out goes
  straight back in.
- `clay_layer_node_params` — the primitive's parameter block, by the
  size-query pattern `clay_layer_children` uses, since the count is a property
  of the primitive and a caller that has just learned the primitive from
  `clay_layer_node_prim` should not have to carry a table of arities to size a
  buffer.
- `clay_layer_node_op_blend` — op, blend profile, blend radius and rounding,
  the four arguments `clay_layer_set_op_blend` takes.

Reading is not editing, so a ghosted, locked or hidden layer answers normally,
as it does for every other reader.

A GROUP has no transform of its own and carries no primitive, so the first two
refuse one exactly as their setter and `clay_layer_node_prim` already do. It
does carry an op and a blend — `clay_layer_set_op_blend` writes them — so the
third answers for both kinds.

## Impact

`c-abi` gains the reading half of a surface it already writes. Purely
additive: no existing signature changes, no struct grows, no enumerator moves,
and no `.clayspace` or scene-format version changes because nothing new is
stored. ABI minor 0.53.0.

`python-bindings` are untouched. The parity gate runs pyclay -> C, so C-only
additions keep it clean; pyclay's own readers are a separate change and are
noted in the tasks rather than smuggled in here.
