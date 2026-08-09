# c-abi — refusals at the host boundary

Delta for `harden-core-boundaries`.

## ADDED Requirements

### Requirement: Replacing a primitive refuses the kinds that carry out-of-line data
`clay_layer_set_prim` SHALL refuse a stroke, a lift, a loft, a sweep or a volume, with `CLAY_ERROR_INVALID_ARGUMENT`. That entry point replaces a node's primitive alone and has no way to supply the payload those kinds read, so accepting one leaves a node the evaluator cannot evaluate.

This is the refusal `clay_add_item` already makes for the same set through the flat descriptor.

#### Scenario: A node cannot be turned into a loft
- **WHEN** `clay_layer_set_prim` is called with `CLAY_PRIM_LOFT`
- **THEN** it returns `CLAY_ERROR_INVALID_ARGUMENT` and the node is unchanged

#### Scenario: The document still evaluates afterwards
- **WHEN** a refused replacement is followed by evaluating the document
- **THEN** evaluation returns the original primitive's field

#### Scenario: An ordinary replacement still works
- **WHEN** `clay_layer_set_prim` is called with a primitive whose parameters fit the params block
- **THEN** it succeeds as before

### Requirement: Every out-of-line count is bounded
A function taking a caller-supplied count and a pointer SHALL check both against the batch ceiling before reserving for them, so that a bogus count is refused rather than reaching the allocator. An allocation failure cannot be reported across this boundary: the library builds without exceptions, so it would end the host process.

#### Scenario: A tube with an impossible point count
- **WHEN** `clay_tube_create` is given a count above the batch limit
- **THEN** it returns null with `CLAY_ERROR_INVALID_ARGUMENT` and the host continues

### Requirement: A requested meshing resolution is priced before it is allocated
`clay_document_mesh` SHALL reject a voxel size that is not finite and positive, and SHALL reject a resolution whose implied dense sample grid exceeds the batch ceiling, before any meshing begins.

#### Scenario: An over-fine resolution is refused
- **WHEN** `clay_document_mesh` is asked for a voxel size that implies more than the ceiling of grid samples over the scene bounds
- **THEN** it returns `CLAY_ERROR_INVALID_ARGUMENT` rather than attempting the allocation

#### Scenario: A sane resolution still meshes
- **WHEN** `clay_document_mesh` is asked for an ordinary resolution
- **THEN** it meshes as before

#### Scenario: The documented resolution is not refused
- **WHEN** `clay_document_mesh` is asked for the resolution the library's own documentation advertises
- **THEN** it meshes

The ceiling SHALL be the mesher's own limit rather than the batch limit: the batch limit bounds how many items cross the boundary in one call, which is a different quantity and far below what this call legitimately needs.

### Requirement: Mirroring a layer is an ordinary layer edit
`clay_set_layer_mirror` SHALL apply through the command vocabulary, so that it refuses a protected layer as every other layer edit does and is recorded on the undo stack.

#### Scenario: A locked layer refuses a mirror
- **WHEN** `clay_set_layer_mirror` names a locked layer
- **THEN** it returns `CLAY_ERROR_INVALID_ARGUMENT` and the layer is unchanged

#### Scenario: A mirror can be undone
- **WHEN** a mirror is set with undo enabled and then undone
- **THEN** the layer returns to its previous mirror state

### Requirement: Moving a layer is one undo step
`clay_document_move_layer` SHALL group the removal and reinsertion it performs, so that a single undo restores the previous order with every layer still present.

#### Scenario: One undo restores the order
- **WHEN** a layer is moved with undo enabled and undone once
- **THEN** the document has the same layers it had before the move, in the same order
