## ADDED Requirements

### Requirement: Voxel mask extrude thickness follows the surface
A voxel layer SHALL extrude the masked cells of the source's surface by the requested amount in cell space. The mask SHALL select surface seeds; its own depth away from the surface SHALL NOT cap the grown wall. The new grid SHALL carry the source's colours, and the source and mask SHALL remain unchanged.

#### Scenario: Wall exceeds mask depth
- **WHEN** a voxel surface is masked in a thin painted band and extruded outward farther than the band reaches
- **THEN** the new wall reaches the requested thickness to within one voxel throughout the selected patch
