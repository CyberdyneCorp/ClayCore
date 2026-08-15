# voxel-engine — triangles reach the cells in one sampling

Delta for `rasterize-mesh`.

## ADDED Requirements

### Requirement: Rasterizing a triangle mesh
`VoxelGrid` SHALL rasterize a triangle mesh directly into cells, in ONE sampling, without an intermediate field or document.

Membership SHALL be decided at the cell centre by the GENERALIZED WINDING NUMBER, the same sign the mesh-to-field import uses. A mesh with a hole, a flipped normal or a self-intersection SHALL rasterize without inverting a half-space: the sign degrades continuously across an opening rather than flipping, which a ray-parity or nearest-triangle-normal test cannot do.

The region SHALL be OPTIONAL and SHALL default to the mesh's own bounds. A document can be unbounded and a mesh cannot, so the default exists here where it does not for a tape. An explicit region SHALL bound the work, and content outside it SHALL be neither rasterized nor reported as missing.

Colour SHALL come from the mesh's vertex colours where it carries them, read at the closest point on the nearest triangle and quantised to the palette by nearest entry. A mesh carrying no colours SHALL take a single neutral entry rather than a colour invented per cell.

The grid's existing contracts SHALL hold unchanged: the change counter SHALL move only for cells whose value actually changed, so rasterizing the same mesh twice SHALL report no change the second time.

An empty mesh, an empty or infinite region, and a mesh whose every index is out of range SHALL each leave the grid untouched and SHALL NOT be errors.

#### Scenario: The solid the triangles bound is filled
- **WHEN** a closed mesh is rasterized
- **THEN** cells inside it are occupied, cells outside are not, and the occupied volume matches the solid's to within the surface's own half-cell

#### Scenario: A hole degrades the sign rather than inverting it
- **WHEN** a mesh missing one face is rasterized
- **THEN** the interior is still occupied, the exterior is still empty, and most of the solid survives

#### Scenario: One sampling keeps what two lose
- **WHEN** a feature thinner than two cells is rasterized directly and through the field-then-document detour
- **THEN** the direct path occupies at least as many cells, and on a thick shape the two agree to within about a cell of surface

#### Scenario: Colour survives the direct path
- **WHEN** a mesh carrying vertex colours is rasterized
- **THEN** the palette carries the mesh's colours, where the same model through the detour arrives with none because a distance field carries no colour

#### Scenario: Rasterizing twice changes nothing the second time
- **WHEN** the same mesh is rasterized into the same grid twice
- **THEN** the change counter moves on the first call and not on the second

### Requirement: The BVH names the triangle nearest a point
`mesh::Bvh` SHALL answer a closest-point query with the point on the surface, the source triangle carrying it, and that point's barycentric coordinates — so an attribute on the mesh can be read where the query landed.

The unsigned-distance query SHALL be defined in terms of it, so the two cannot disagree about which point is nearest, and its results SHALL be unchanged.

A tie between two equally near triangles SHALL resolve to the one found first, so the answer does not depend on the order the tree happened to be traversed in.

#### Scenario: An attribute can be read off the nearest triangle
- **WHEN** the closest point to a query is on a triangle whose corners carry different colours
- **THEN** the returned barycentrics reconstruct the point from those corners, and interpolating the corner colours by them gives the colour at that point

#### Scenario: Distances did not move
- **WHEN** the same unsigned-distance and winding queries are run before and after this change
- **THEN** the results are identical
