# meshing — attributes are matched by length, not by emptiness

Delta for `harden-core-boundaries`.

## ADDED Requirements

### Requirement: Decimation reads an attribute only when it is aligned
`mesh::decimate` SHALL carry a normal, color or uv array through only when its length equals the position count, in every pass. Testing an attribute for emptiness instead admits a short array and indexes past its end — and a mesh imported from a file can carry one.

#### Scenario: A mesh with a short attribute array
- **WHEN** a mesh whose colors array is shorter than its positions array is decimated
- **THEN** decimation reads nothing out of bounds and drops the unaligned attribute

#### Scenario: An aligned mesh keeps its attributes
- **WHEN** a mesh whose attributes match its position count is decimated
- **THEN** the attributes are carried through as before

### Requirement: A mesher prices the grid its resolution implies
`mesh_tape` SHALL reject a voxel size that is not finite and positive, and a resolution whose implied dense lattice exceeds the module's documented sample ceiling, returning an empty mesh rather than sizing the allocation from the caller's number.

The ceiling SHALL admit the resolution the library's documentation advertises; a guard that turns documented usage into an error is a worse defect than the one it prevents.

#### Scenario: An over-fine voxel size yields an empty mesh
- **WHEN** `mesh_tape` is called with a voxel size so fine that the region needs more than the ceiling of lattice points
- **THEN** it returns an empty mesh and does not allocate the lattice

#### Scenario: A non-finite voxel size yields an empty mesh
- **WHEN** `mesh_tape` is called with a voxel size of zero, a negative, an infinity or a not-a-number
- **THEN** it returns an empty mesh
