# voxel-engine — a grid off disk has a usable cell size

Delta for `harden-core-boundaries`.

## ADDED Requirements

### Requirement: A deserialized grid's voxel size is a positive real
`VoxelGrid::deserialize` SHALL refuse a payload whose voxel size is zero, negative, infinite or not a number. Every world-to-cell conversion divides by that size and casts the result to a 32-bit integer, which is undefined for a non-finite quotient; a document loaded from disk is enough to reach it.

`MaskField::deserialize` SHALL apply the same rule to its cell size, infinity included.

#### Scenario: A grid declaring a zero voxel size
- **WHEN** a voxel payload carrying a voxel size of zero is deserialized
- **THEN** it is refused

#### Scenario: A grid declaring a non-finite voxel size
- **WHEN** a voxel payload carrying an infinite or not-a-number voxel size is deserialized
- **THEN** it is refused

#### Scenario: An ordinary grid round-trips
- **WHEN** a grid with a positive voxel size is serialized and read back
- **THEN** it round-trips losslessly, as before

### Requirement: Sparse operations cost the material, not its bounding box
Greedy meshing and the mask region operations SHALL cost time and memory in proportion to the cells that carry material, not to the bounding box enclosing them. A grid and a mask are sparse by construction and a bounding box is not: material far apart on two or more axes makes the box enormous while the material stays small.

Neither SHALL size a buffer from a difference of two lattice coordinates, which overflows a 32-bit integer for coordinates a deserialized grid or mask may legitimately carry, and would then ask for an allocation no allocator can satisfy — which ends the process rather than returning, since the library builds without exceptions.

The output SHALL be unchanged. Merging still spans whatever the dense sweep merged, and a neighbourhood is still clamped at the padded bounding box rather than at any internal block edge.

#### Scenario: Two voxels far apart mesh immediately
- **WHEN** a grid holding two voxels separated by thousands of cells on every axis is greedily meshed
- **THEN** it returns the twelve quads those two cubes expose, in time proportional to the two cubes

#### Scenario: Connected material merges as before
- **WHEN** a solid block is greedily meshed
- **THEN** each of its faces merges into the single quad it always did

#### Scenario: Two painted blobs far apart expand immediately
- **WHEN** a mask carrying two small painted blobs separated by thousands of cells is expanded
- **THEN** both blobs grow, the space between them is untouched, and the call costs the painted cells

#### Scenario: A compact mask is not made slower
- **WHEN** a mask whose paint fits inside a single chunk is expanded
- **THEN** it costs no more than the region its paint occupies
