# Record the layer a crossing creates

## Why

`unify-the-undo-history` made the *filling* of a layer undoable. Layer creation
stayed unrecorded for one kind of layer, and the combination is worse than
either half: a crossing — make a voxel layer, rasterize a starting form into it
— recorded its fill and not its layer, so one undo emptied the new layer and
left it standing.

Measured by the host that filed #341. Same document, same operation, same single
undo, only the engine tag changed:

| | v0.39.0 | v0.52.2 |
|---|---|---|
| undo depth after the crossing | 1 (unchanged) | 2 |
| layers after undo | 2 | 2 |
| visible vertices after undo | 3,952 | **0** |

On v0.39.0 the crossing recorded nothing, so undo stepped past it. On v0.52.2 it
records the fill alone, and undoing that empties the layer while leaving it in
the document. An empty layer nobody asked for is not "taken back".

The obvious host-side repair does not work, and the ABI's own contract is why:
`clay_document_remove_layer` records too, so the repair is itself an undo step.
The host that filed this ended up carrying a shadow layer list and dropping the
layer at save.

## What was actually wrong

Two defects, and fixing either alone leaves the bug.

**1. One reachable layer creation escaped the command vocabulary.**
`clay_add_sdf_layer` and `clay_document_add_mesh_layer` both reserve an id and
apply `AddLayerCmd`; `clay_document_add_voxel_layer` mutated the document
directly. `reserve_layer_id`'s own comment says it exists "for bindings that add
layers through the command vocabulary so the add is undoable" — this binding did
not use it. `pyclay`'s `Document.add_voxel_layer` had the same gap, so the
python-bindings claim that no reachable edit escapes undo was false there too.

**2. An undo group did not group.** `History::begin_group` forwards to
`UndoStack::begin_group` and nothing else, so it bundles SCENE COMMANDS and only
those. A voxel, mask or mesh step recorded between the brackets was pushed
straight onto the step list and stayed its own undo. `clay.h` has promised
"bracket a burst of edits so they undo as one step" since the bracket shipped,
and for every representation but the edit list the promise was false.

Recording the creation without fixing the bracket makes the crossing *two*
steps whose first undo removes the layer out from under the fill it contains —
strictly worse than today. Both halves are this change.

## What changes

- `clay_document_add_voxel_layer` and `pyclay`'s `Document.add_voxel_layer` go
  through `AddLayerCmd`, like their two siblings.
- `Step::Kind::Compound`: an explicit `begin_group`/`end_group` bracket folds
  into one step across every representation. A bracket of commands alone is
  unchanged, and a bracket holding a barrier is left alone.
- Saving filters voxel chunks by their layer, and loading drops unmatched ones —
  the rule mesh layers already had, deferred at the time as
  `add-mesh-layers` task 7.7 and now reachable.

## What was rejected

The issue offered three fixes and ranked them. Options 2 and 3 were both
declined:

- **An unrecorded layer removal** (issue option 2) is an escape hatch from
  "every editing entry point records its own inverse, so no reachable edit
  escapes undo". It makes the host's repair correct at the cost of the sentence
  that made the repair necessary, and every host still has to know to call it.
- **Stop recording the rasterization** (issue option 3) restores v0.39.0 for
  this path by giving back what `unify-the-undo-history` won.

## Impact

- Affected specs: `c-abi`, `scene-model`, `file-io`, `python-bindings`
- A host that counted undo steps per gesture must recount: a bracket that
  produced several steps now produces one. This is the promise `clay.h` already
  made, so a host that read the header rather than measured the behaviour needs
  no change.
- A saved document no longer carries voxel chunks for layers it does not
  contain, and one that already does loses them on load.
