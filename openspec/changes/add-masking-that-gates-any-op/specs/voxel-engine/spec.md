# voxel-engine — masking that gates any operation

Delta for `add-masking-that-gates-any-op`.

## ADDED Requirements

### Requirement: A painted mask can gate an SDF operation
A painted `MaskField` SHALL be convertible into the form an SDF item's gate consumes, so the mask an artist paints protects both representations rather than only the voxel one.

The two SHALL agree: a region protected from a voxel edit SHALL be protected from the equivalent SDF operation.

#### Scenario: The same painted mask protects both representations
- **WHEN** the same mask gates a voxel edit and the equivalent SDF operation
- **THEN** the protected region is unchanged in both
