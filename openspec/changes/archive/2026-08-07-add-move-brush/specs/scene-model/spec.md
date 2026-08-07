# scene-model — changing a node's deformers

Delta for `add-move-brush`.

## ADDED Requirements

### Requirement: A node's deformer chain is editable through the command vocabulary
The command vocabulary SHALL be able to replace a node's deformer chain, as it can already replace that node's transform, primitive, colour, op and stroke points. The replacement SHALL be of the WHOLE list, and its inverse SHALL be the list that was there before.

Without it a deformer can only be set when a node is created, so no verb built on deformers can act on an existing sculpt — and any that tried would escape undo, which every other destructive operation is required not to do.

A whole-list replace is chosen over granular add and remove for the reason `SetStrokePointsCmd` was: a chain is a handful of records, so replacing it costs less than the commands to edit it would, and its inverse is exact by construction rather than by reconstruction.

#### Scenario: A chain is replaced and undone
- **WHEN** a node's deformers are replaced and the edit is undone
- **THEN** the node evaluates exactly as it did before the replacement

#### Scenario: The chain survives the document format
- **WHEN** a document whose node has a replaced deformer chain is saved and reloaded
- **THEN** it evaluates identically

#### Scenario: A missing node is refused
- **WHEN** the command names a node or layer that does not exist
- **THEN** it is refused rather than silently doing nothing

### Requirement: Deformer order is part of the contract
A node's deformers SHALL apply in authoring order, with `deformers[0]` warping the point first, so that the FIRST entry is the outermost warp on the resulting geometry and the last is the one nearest the primitive.

This is already what the evaluator does; stating it makes it something a caller may rely on. A verb that warps the assembled shape SHALL therefore insert its deformer at the FRONT of the chain, because one appended at the back has its region weight evaluated at a point the earlier deformers have already moved — and so acts somewhere other than where the caller aimed it.

#### Scenario: Position in the chain changes the result
- **WHEN** the same two deformers are applied to one item in both orders
- **THEN** the resulting fields differ

#### Scenario: A prepended warp acts where it was aimed
- **WHEN** a region warp is prepended to a chain whose existing deformer moves the region
- **THEN** the warp acts at the position the caller gave, not at the moved one
