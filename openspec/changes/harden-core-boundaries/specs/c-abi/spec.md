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
