# Proposal: one sculpt-to-layer conversion is one undo step (#656)

## Why

`clay_voxel_to_layer` recorded **two** undo steps: `clay_add_sdf_layer` and
then `insert_node`, each its own `apply_edit`, with nothing bracketing them.
A host that maps one user action to one engine undo — ClaySpaceDesktop does,
and grid mask extrude goes through this call — got a two-press undo for every
grid-to-field crossing. The first press removed the volume and left an EMPTY
layer standing; the redo after it reported that nothing had changed.

Measured on `main` (0f45417d) with the regression test below, undo enabled,
9³-cell single-colour grid:

| | before | after |
|---|---|---|
| undo depth across one call | +2 | +1 |
| layer count after one undo | 1 (empty) | 0 |
| one redo | the volume only | the layer with its one volume item |

The live `c-abi` spec also contradicts itself. "A host converts a sculpt into a
layer it can keep working on" still says the call places "one volume item per
palette entry", while the later "A host converts a coloured sculpt in one call"
says it produces a single item — which is what the code does. `clay.h` carried
the same stale text, and described the call as `clay_item_volume_from_voxels`
"in a loop".

## What changes

- `clay_voxel_to_layer` adds the layer ALREADY HOLDING its volume item, as one
  `AddLayerCmd`. One command is one undo step without a bracket, and it is
  atomic: there is no state between "layer added" and "item added" for a
  failure to strand, so the promise that a failed conversion leaves the
  document as it was holds by construction rather than by a cleanup path.
- The requirement is restated to describe the single palette-carrying volume
  and to require that the conversion is one undo step, with a scenario for it.
- `clay.h` is rewritten to match.

## What building it found

The suggested fix was to bracket the two existing edits with
`begin_group`/`end_group`, as `clay_document_move_layer` does, and on an
`insert_node` failure remove the layer before closing the group. That works
for the success path, but the failure path then records an add and a remove
inside one bracket — a step that does nothing, left in the host's undo menu.
Building the layer with its content and adding it once has no failure path
between the two halves at all, and is less code. `AddLayerCmd` already carries
a populated layer by value (a layer reorder is exactly that), so its inverse,
its journal form and its invalidation bound — taken on both sides of the
apply — need nothing new.

## What this does NOT change

- No entry point and no signature changes: no ABI version bump.
- The document format is untouched; the result is still an ordinary volume
  item in an ordinary SDF layer.
- `clay_item_volume_from_voxels` is unchanged.
