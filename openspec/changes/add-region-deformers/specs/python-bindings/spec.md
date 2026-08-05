# python-bindings — grab and pose

Delta for `add-region-deformers`.

## ADDED Requirements

### Requirement: grab and pose from Python
`Prim.grab(center, radius, displacement, ease=0, front_facing=False)` and `Prim.pose(center, radius, ...)` SHALL append the corresponding deformer, and `VoxelGrid.grab(cell, radius, displacement, ...)` SHALL apply the voxel verb. All SHALL compose in call order and survive a `.clayspace` round trip.

#### Scenario: Grabbing from Python
- **WHEN** a script grabs part of a primitive
- **THEN** the field changes only within the radius

#### Scenario: A non-positive radius is refused
- **WHEN** the radius is zero or negative
- **THEN** the call raises, since the falloff would divide by zero
