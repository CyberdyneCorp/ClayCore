# c-abi — voxels, picking, evaluation parity

Delta for `widen-c-abi-voxels`.

## ADDED Requirements

### Requirement: Voxel grids across the ABI
The API SHALL expose voxel grids through an opaque handle: palette management, single-cell and batch edits, cube and sphere brushes with falloff and strength, the sculpting verbs (smooth, inflate, flatten, pinch), box and line fills, mirrored edits, flood select, occupancy and bounds queries, greedy meshing, SDF rasterization, and step-field sampling.

Ownership SHALL be explicit: a grid created standalone is owned by the caller and destroyed with an explicit destroy call, while a grid obtained as a document layer is borrowed, remains owned by the document, and SHALL NOT be destroyed by the caller. Destroying a borrowed handle SHALL return an error rather than corrupting the document.

#### Scenario: Voxel sculpting from C
- **WHEN** a C consumer creates a grid, adds palette entries, stamps a sphere brush, runs a sculpting verb, and greedy-meshes the result
- **THEN** the mesh matches the same sequence performed through `pyclay`

#### Scenario: Borrowed layer handle is protected
- **WHEN** a consumer calls destroy on a handle obtained from a document voxel layer
- **THEN** the call returns an invalid-argument error and the document is unaffected

#### Scenario: Batch edits use the size-query pattern
- **WHEN** flood select is called with a null buffer
- **THEN** it reports the required cell count, and a second call with an adequate buffer fills it

#### Scenario: Falloff brushes are reproducible across the boundary
- **WHEN** a C consumer stamps a falloff brush with a given seed
- **THEN** the affected cells are identical to the same stamp through `pyclay`

#### Scenario: Brush strength is passed through, never reinterpreted
- **WHEN** a C consumer stamps a brush at any strength the boundary accepts
- **THEN** the coverage reaches the engine untouched, so the affected cells are identical to the same stamp through `pyclay`
- **AND WHEN** the strength is not greater than zero, which covers no cell at all
- **THEN** the call returns an invalid-argument error and the grid is unchanged, rather than the value being read as full coverage

#### Scenario: A region with a non-finite bound is refused
- **WHEN** rasterization is asked for a region whose bounds contain a NaN or an infinity
- **THEN** the call returns an invalid-argument error and the grid is unchanged

### Requirement: Picking and evaluation parity
The API SHALL expose gradients, field colours, batch raycast, safe step scale, surface snapping, layer bounds, selection bounds, voxel cell/face picking, build-plane picking, and raycast that attributes the hit to a layer and node. Mesh generation SHALL allow selecting the mesher, with the experimental one reachable only behind its explicit flag.

#### Scenario: Zoom to selection from C
- **WHEN** a consumer requests the bounds of a selection of nodes
- **THEN** it receives the same AABB the Python bindings report for that selection

#### Scenario: Attributed raycast
- **WHEN** a raycast hits an item in a multi-layer document
- **THEN** the call reports the layer and node identifiers alongside the hit position and normal

#### Scenario: Experimental mesher stays gated
- **WHEN** a consumer selects dual contouring without setting the experimental flag
- **THEN** the call returns an error rather than silently meshing

### Requirement: Batch counts are bounded
Every entry point taking an element count SHALL reject a count above a documented ceiling with an invalid-argument error rather than sizing a buffer from it. A count is the one argument the boundary cannot check against the caller's memory, and the library builds without exceptions, so an allocation failure would terminate the host process instead of returning an error code.

#### Scenario: A count that is not a count
- **WHEN** a consumer passes a byte length, or a negative signed value widened to `size_t`, where an element count belongs
- **THEN** the call returns an invalid-argument error and the process survives

### Requirement: Binding parity gate
The repository SHALL gate that the C ABI reaches the capability surface the Python bindings reach. A capability exposed by `pyclay` without a corresponding C entry point SHALL fail the gate unless it carries a recorded exemption naming why.

#### Scenario: A new binding without a C entry point
- **WHEN** a capability is added to `pyclay` and no C entry point is added
- **THEN** the parity gate fails in CI naming the missing capability

#### Scenario: Recorded exemption
- **WHEN** a capability is deliberately Python-only and carries an exemption
- **THEN** the gate passes and the exemption is visible in the gate's output

#### Scenario: A declaration without a definition
- **WHEN** `clay.h` declares an entry point the shared library does not export
- **THEN** the ABI gate fails naming it, rather than leaving the link error for every generated binding to discover
