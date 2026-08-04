# voxel-engine — brush shapes

Delta for `add-brush-shapes`.

## MODIFIED Requirements

### Requirement: Voxel editing operations
The module SHALL provide: set/erase/paint of single voxels and of brush footprints, box fills, line fills, per-axis mirror application of any edit, build-plane queries (which cell/face a ray hits at a given plane), and flood select (connected voxels by color/solidity).

Brush footprints SHALL support a shape: a solid N×N×N cube, or the sphere inscribed in that cube — cells whose centre lies within radius `(N − 1) / 2` of the brush centre, which makes the sphere always a subset of the cube of the same size. Cube SHALL remain the default so existing calls are unaffected. Because the radius is `(N − 1) / 2`, an even N SHALL behave as N − 1.

#### Scenario: Mirrored edit
- **WHEN** a voxel is placed at (x,y,z) with X-mirror active
- **THEN** the mirrored voxel is also placed at (−x,y,z) in the same undoable command

#### Scenario: Flood select respects connectivity
- **WHEN** flood select starts on a voxel of color c
- **THEN** exactly the 6-connected voxels of color c reachable from the seed are selected

#### Scenario: Cube brush
- **WHEN** a size-3 cube brush is stamped
- **THEN** all 27 cells of the 3×3×3 block are set

#### Scenario: Sphere brush
- **WHEN** a size-N sphere brush is stamped
- **THEN** only cells within radius `(N − 1) / 2` of the centre are set, and the result is a subset of the cube brush of the same size

#### Scenario: Paint brush leaves empty cells alone
- **WHEN** a paint brush of either shape covers a region containing empty cells
- **THEN** occupied cells in the footprint are recoloured and no new cells are created

#### Scenario: Even sizes round down
- **WHEN** a brush of size 4 is stamped
- **THEN** it covers the same cells as size 3
