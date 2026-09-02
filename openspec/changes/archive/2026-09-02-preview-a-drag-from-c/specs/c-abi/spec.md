## ADDED Requirements

### Requirement: A Move drag previews as an ordinary document from C
The C ABI SHALL expose a live Move transaction's preview as a borrowed, read-only `clay_document` carrying the real document's layers with the dragged one replaced by the preview. It SHALL be usable wherever a document is — evaluation, meshing, picking, brick refill — so a host draws the drag through machinery it already has, and the real document SHALL NOT be modified.

The handle SHALL be valid until the transaction is committed, cancelled or destroyed, and SHALL be NULL for a spent transaction. It SHALL share the transaction's content, so an update is visible through it without a refresh, and the document's compiled-tape cache SHALL be invalidated on each request, since the drag changes the edit list without going through a mutating entry point.

It SHALL carry the SDF layers. Voxel grids, masks and mesh layers are not part of the field tape, so their absence changes nothing that reads the field, and copying them would charge a drag for content it cannot change.

A drag that reaches no items SHALL still preview, as the layer unchanged.

#### Scenario: The preview carries the drag and the document does not move
- **WHEN** a live drag is updated and the preview document is evaluated
- **THEN** the surface shows the drag, the real document's field is unchanged, and its saved bytes are identical

#### Scenario: A second update is visible through the same handle
- **WHEN** the transaction is updated again and the preview document evaluated
- **THEN** it shows the newer drag rather than the previous frame's

#### Scenario: The preview is what the commit writes
- **WHEN** the drag is committed
- **THEN** the real document's field matches what the preview last showed

#### Scenario: The preview dies with the gesture
- **WHEN** the transaction is committed or cancelled
- **THEN** the preview document is no longer available

### Requirement: A placed deformer can be taken back
The C ABI SHALL offer an inverse for `clay_layer_add_deformer`: a count, a remove by index, and a clear. Undo SHALL NOT be the only way to remove a warp, since undoing also spends a history entry the caller never meant to make.

All three SHALL be undoable edits. Removing at an index past the end SHALL be refused rather than silently ignored; clearing a chain that is already empty SHALL succeed.

#### Scenario: A chain can be counted, shortened and emptied
- **WHEN** two deformers are added to a placed node and one is removed by index
- **THEN** the count reports two, then one, and clearing takes it to zero

#### Scenario: An index past the end is refused
- **WHEN** remove names an index the chain does not have
- **THEN** it is refused and the chain is unchanged

#### Scenario: Removing is undoable
- **WHEN** a deformer is removed and the edit undone
- **THEN** the chain is exactly what it was
