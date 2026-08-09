# c-abi — masks on the SDF side

Delta for `add-sdf-masking`.

## ADDED Requirements

### Requirement: SDF masking across the ABI
The C API SHALL expose associating a mask with an SDF layer and the mask-driven edits, reusing the existing `clay_mask` handle rather than introducing a second mask type. The addition SHALL be purely additive.

#### Scenario: The same mask handle serves both layer kinds
- **WHEN** a `clay_mask` built for a voxel layer is passed to an SDF mask-driven edit
- **THEN** it is accepted and selects the same world region
