## MODIFIED Requirements

### Requirement: Voxel grab moves occupancy through the same map
The voxel engine SHALL provide a grab verb taking the same centre, radius, displacement and falloff as the SDF deformer, resampling occupancy and palette index from the inverse-displaced position so both representations mean the same thing.

Because occupancy is binary, resampling SHALL be nearest-cell, and the spec SHALL state plainly that a displacement larger than a cell aliases: material moves in whole cells, and a slow drag will step rather than flow. This is a property of the representation, not a defect to be hidden.

The grab SHALL weight its map with the SAME falloff table every other voxel verb reads, so a `BrushFalloff` names one curve across the whole voxel engine. It SHALL NOT reinterpret the falloff as an easing index: the two enumerations do not correspond, and doing so gave each falloff the next one's curve while leaving `Constant` unreachable.

#### Scenario: Material moves with the pull
- **WHEN** a voxel grab displaces a region
- **THEN** cells in the direction of the displacement become occupied and cells behind it are vacated, with colour carried along

#### Scenario: Outside the radius nothing changes
- **WHEN** a voxel grab is applied
- **THEN** no cell beyond the radius from the centre changes occupancy or index

#### Scenario: Grab agrees with the SDF deformer in shape
- **GIVEN** a falloff an easing curve can express — `Linear` is ease_linear and `Smooth` is ease_smoothstep, both exactly
- **WHEN** the same centre, radius, displacement and falloff are applied to a voxelized sphere and to the equivalent SDF sphere
- **THEN** the displaced surfaces agree to within the voxel size

#### Scenario: Two falloffs have no SDF equivalent, and that is stated rather than implied
- **GIVEN** `Constant` or `Gaussian`, which no easing curve reproduces because the SDF region weight always carries the (1 - d) factor
- **WHEN** a host compares the two representations
- **THEN** it is told they cannot correspond for those two, instead of being given a curve that silently belongs to a different falloff

#### Scenario: A constant falloff pulls rigidly
- **WHEN** a grab with a constant falloff displaces a region
- **THEN** material half way to the rim moves as far as material at the centre, within a cell of quantisation

#### Scenario: A tapering falloff is distinguishable from a constant one
- **WHEN** the same grab is applied with a constant and with a linear falloff
- **THEN** the constant one moves material at half the radius measurably further, and the linear one still moves the centre further than the rim
