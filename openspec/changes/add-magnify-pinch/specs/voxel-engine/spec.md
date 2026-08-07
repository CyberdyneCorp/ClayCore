# voxel-engine — magnify

Delta for `add-magnify-pinch`.

## ADDED Requirements

### Requirement: Voxels can magnify as well as pinch
Voxel layers have `sculpt_pinch`, which moves surface cells one step toward the brush centre. They SHALL also be able to move them one step away from it, so that the two representations agree on what the verb means — a document must not mean something different depending on which one it is stored in.

#### Scenario: Magnify moves surface cells outward
- **WHEN** a voxel shape is magnified about a point inside it
- **THEN** surface cells near that point move away from the centre

#### Scenario: It is the inverse of pinch
- **WHEN** a shape is pinched and then magnified with the same brush
- **THEN** the result is closer to the original than either operation alone
