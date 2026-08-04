# voxel-engine — brush size means N cells

Delta for `fix-brush-even-sizes`.

## MODIFIED Requirements

### Requirement: Voxel editing operations
The module SHALL provide: set/erase/paint of single voxels and of brush footprints, box fills, line fills, per-axis mirror application of any edit, build-plane queries (which cell/face a ray hits at a given plane), and flood select (connected voxels by color/solidity).

Brush footprints SHALL support a shape: a solid cube, or the sphere of the same diameter. A brush of size N SHALL span exactly N cells per axis for every N ≥ 1, with the footprint running from `-((N−1)/2)` to `N/2` inclusive — symmetric for odd N, biased half a cell toward the positive axes for even N. The sphere SHALL admit cells whose centre lies within radius `N/2` of the footprint centre, so that it remains a subset of the cube and its occupancy ratio approaches π/6 as N grows. Cube SHALL remain the default.

#### Scenario: Mirrored edit
- **WHEN** a voxel is placed at (x,y,z) with X-mirror active
- **THEN** the mirrored voxel is also placed at (−x,y,z) in the same undoable command

#### Scenario: Flood select respects connectivity
- **WHEN** flood select starts on a voxel of color c
- **THEN** exactly the 6-connected voxels of color c reachable from the seed are selected

#### Scenario: Cube brush covers N cells per axis
- **WHEN** a cube brush of size N is stamped
- **THEN** it sets exactly N³ cells, for both odd and even N

#### Scenario: Every size is distinct
- **WHEN** brushes of size 3 and size 4 are stamped
- **THEN** size 4 covers strictly more cells than size 3

#### Scenario: Sphere brush
- **WHEN** a size-N sphere brush is stamped
- **THEN** only cells whose centre lies within radius `N/2` of the footprint centre are set, and the result is a subset of the cube brush of the same size

#### Scenario: Sphere is non-degenerate at small even sizes
- **WHEN** a sphere brush of size 2 is stamped
- **THEN** it sets a non-empty footprint

#### Scenario: Paint brush leaves empty cells alone
- **WHEN** a paint brush of either shape covers a region containing empty cells
- **THEN** occupied cells in the footprint are recoloured and no new cells are created
