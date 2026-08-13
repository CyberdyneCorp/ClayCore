# voxel-engine — the sculpt comes back

Delta for `voxel-to-field` (#90).

## ADDED Requirements

### Requirement: A grid converts to a field without a mesh in between
A grid SHALL convert directly to a sampled field, so a sculpt can re-enter the document as an operand. The existing route — mesh the grid, sample the triangles — resamples twice, builds a BVH to do it, and drops the palette, and SHALL NOT be the only way back.

The conversion SHALL locate the surface by reading occupancy with trilinear interpolation between cell CENTRES rather than as cells, so the isosurface is a surface and not a staircase. Nothing SHALL be filtered by default, so no occupied cell disappears; an optional blur MAY be offered and SHALL be documented as costing thin features.

The result SHALL be redistanced, so its stored values are the distance to their own zero set and it carries a Lipschitz bound. A field whose values are an occupancy ramp crosses zero in the right place and says nothing truthful about distance, and every marcher and blend downstream would be working from a guess.

The conversion SHALL accept ONE palette index, converting only the cells carrying it. A field has nowhere to store a palette, so converting per entry is what lets colour survive: the parts placed together are the whole solid and the interface between two colours is interior to their union.

The conversion SHALL NOT modify the grid.

The conversion SHALL be documented as LOSSY IN BOTH DIRECTIONS. Going to voxels quantises to the lattice and no care on the way back recovers a boolean's sharp edge; coming back turns binary occupancy into a distance. What is preserved is the surface within about a cell and the colour; what is not is exactness and the procedural history.

#### Scenario: A sculpt makes the return trip
- **WHEN** a document is blocked out with booleans, rasterized into a grid, sculpted with the voxel verbs, and converted back
- **THEN** the result is an operand: it can be placed in a layer and booleaned against, and it evaluates solid where the sculpt is and carved where a subtraction was placed

#### Scenario: The surface comes back within a cell
- **WHEN** a ball of voxels is converted and the field is sampled where the ball's surface is, on and off axis
- **THEN** the field reads within about a cell of zero at every such point

#### Scenario: The field measures distance, not occupancy
- **WHEN** a converted field is sampled deep inside the solid
- **THEN** the value is approximately the distance to the surface rather than a fraction of an occupancy step

#### Scenario: One palette entry converts alone
- **WHEN** a grid carrying two palette entries is converted once per entry
- **THEN** each result is solid only where its own entry's cells are, and an entry the grid does not carry converts to nothing
