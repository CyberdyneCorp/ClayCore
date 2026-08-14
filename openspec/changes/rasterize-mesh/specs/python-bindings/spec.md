# python-bindings — rasterize_mesh from Python

Delta for `rasterize-mesh`.

## ADDED Requirements

### Requirement: VoxelGrid.rasterize_mesh
`pyclay` SHALL expose mesh rasterization on `VoxelGrid`, taking a `Mesh` and an optional region, with the region defaulting to the mesh's own bounds.

Invalid arguments SHALL raise rather than be clamped, matching the C ABI's refusals so the two do not disagree about what a value means.

The GIL SHALL be released around the work, as it is for the other heavy grid calls.

#### Scenario: An imported model reaches the voxel verbs
- **WHEN** a mesh is loaded and rasterized, and a sculpting verb is then applied
- **THEN** the grid is occupied and the verb changes cells, with no document constructed on the way

#### Scenario: The bindings agree with the ABI about a refusal
- **WHEN** an inverted or malformed region is passed
- **THEN** Python raises and C returns `CLAY_ERROR_INVALID_ARGUMENT`

### Requirement: Binding parity holds
`tools/check_binding_parity.py` SHALL pass with the capability reachable from the C ABI.

#### Scenario: The gate passes
- **WHEN** the parity gate runs against the built module and `clay.h`
- **THEN** it reports no unmatched capability and no stale exemption
