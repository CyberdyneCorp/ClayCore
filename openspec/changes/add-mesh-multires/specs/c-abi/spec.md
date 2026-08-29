# c-abi — multiresolution surfaces

Delta for `add-mesh-multires`.

## ADDED Requirements

### Requirement: A host can drive a hierarchy across the ABI
The C ABI SHALL expose the multiresolution surface as an opaque handle with level creation and removal, independent sculpt and display levels, sculpting at the active level, and export of any level as a mesh.

Adding a level SHALL report its predicted cost and SHALL fail with a typed budget error rather than allocating part of it.

The ABI SHALL report revisions for the base, the detail and the evaluated surface separately, and SHALL expose changed blocks with caller-owned buffers and a capacity query rather than copying a display-level mesh per stamp.

Descriptors SHALL follow the established `struct_size` pattern with bounded output fills, and long operations SHALL accept the cancellation token.

#### Scenario: A detail stamp does not copy the display mesh
- **WHEN** a host stamps detail on a deep hierarchy and drains the changed blocks
- **THEN** the bytes copied follow the changed blocks rather than the display level's size

#### Scenario: An over-budget level is refused across the ABI
- **WHEN** a host requests a level whose predicted cost exceeds the budget it declared
- **THEN** the call returns a typed budget error and the surface is unchanged
