# voxel-engine — brush shapes

Delta for `add-brush-shapes`.

## MODIFIED Requirements

### Requirement: Editing operations
The module SHALL provide: set/erase/paint of single voxels and of brush footprints, box fills, line fills, per-axis mirror application of any edit, build-plane queries (which cell/face a ray hits at a given plane), and flood select (connected voxels by color/solidity).

Brush footprints SHALL support a shape: a solid `n x n x n` cube, or the sphere inscribed in that cube — cells whose centre lies within radius `(n - 1) / 2` of the brush centre. Cube SHALL remain the default so existing calls are unaffected. Because the radius is `(n - 1) / 2`, an even `n` SHALL behave as `n - 1`.

#### Scenario: Cube brush
- **WHEN** a size-3 cube brush is stamped
- **THEN** all 27 cells of the 3x3x3 block are set

#### Scenario: Sphere brush
- **WHEN** a size-n sphere brush is stamped
- **THEN** only cells within radius `(n - 1) / 2` of the centre are set, and the result is a subset of the cube brush of the same size

#### Scenario: Paint brush leaves empty cells alone
- **WHEN** a paint brush of either shape covers a region containing empty cells
- **THEN** occupied cells in the footprint are recoloured and no new cells are created

#### Scenario: Even sizes round down
- **WHEN** a brush of size 4 is stamped
- **THEN** it covers the same cells as size 3
