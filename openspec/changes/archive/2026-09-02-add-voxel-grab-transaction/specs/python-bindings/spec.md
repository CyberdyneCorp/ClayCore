## ADDED Requirements

### Requirement: A voxel drag is a context manager in pyclay
`VoxelGrid.grab` SHALL return a gesture usable as a context manager: leaving the block commits it and an exception cancels it. `update` SHALL take the total displacement from the anchor, and `written_box` SHALL report the footprint the gesture writes.

#### Scenario: However the drag is delivered, it lands the same
- **WHEN** the same total drag is delivered as one, two, four and eight updates
- **THEN** the occupied cells are identical, and differ from the untouched grid

#### Scenario: An exception inside the block cancels
- **WHEN** the block raises after an update
- **THEN** the material is what it was when the gesture began
