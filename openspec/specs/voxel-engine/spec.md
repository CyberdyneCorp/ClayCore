# voxel-engine Specification

## Purpose
TBD - created by archiving change add-claycore-v1. Update Purpose after archive.
## Requirements
### Requirement: Voxel storage
`clay::voxel` SHALL store colored voxel grids as palette-indexed dense chunks scaling to at least 256³ per layer, with palette + RLE compression for serialization. Voxel color SHALL be a palette index; palette edits SHALL recolor all referencing voxels without touching voxel data.

#### Scenario: 256³ grid within budget
- **WHEN** a 256³ layer with typical sparse occupancy is stored and serialized
- **THEN** in-memory chunks allocate only where voxels exist and the serialized form is palette+RLE compressed

### Requirement: Voxel editing operations
The module SHALL provide: set/erase/paint of single voxels and N×N×N brush footprints, box fills, line fills, per-axis mirror application of any edit, build-plane queries (which cell/face a ray hits at a given plane), and flood select (connected voxels by color/solidity).

#### Scenario: Mirrored edit
- **WHEN** a voxel is placed at (x,y,z) with X-mirror active
- **THEN** the mirrored voxel is also placed at (−x,y,z) in the same undoable command

#### Scenario: Flood select respects connectivity
- **WHEN** flood select starts on a voxel of color c
- **THEN** exactly the 6-connected voxels of color c reachable from the seed are selected

### Requirement: Greedy meshing
The module SHALL mesh voxel grids with greedy face merging, preserving per-face color (no color bleeding across merged faces), producing meshes suitable for both viewport preview and export.

#### Scenario: Greedy merge is lossless
- **WHEN** a voxel grid is greedy-meshed
- **THEN** the mesh's rasterized surface equals the per-voxel-face surface exactly (same faces covered, same colors), with strictly fewer or equal quads

### Requirement: Voxel–SDF bridges
The module SHALL expose a voxel grid as a step-function field usable in SDF compositing (with documented non-exact/bound classification), and SHALL rasterize any SDF layer into voxels at a caller-chosen resolution with nearest-color assignment.

#### Scenario: SDF to voxels round trip
- **WHEN** an SDF sphere is rasterized into voxels at resolution r
- **THEN** every voxel whose center is inside the sphere is set (and no voxel whose center is outside), with color sampled from the SDF color field

