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
