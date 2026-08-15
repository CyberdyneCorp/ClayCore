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

<!-- Binding parity is NOT restated here. `mesh-fixed-topology-brushes` added
     "Binding parity holds" to this capability's living spec, and a delta that
     re-adds an existing requirement is refused on archive — correctly: the
     requirement is standing, and every change owes it rather than each one
     declaring its own copy. -->
