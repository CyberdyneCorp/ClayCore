# python-bindings — the mask field

Delta for `add-mask-field`.

## ADDED Requirements

### Requirement: Masks from Python
The module SHALL expose a mask on a layer: enabling it, painting with the brush vocabulary, the region operations, sampling at an (N, 3) array of world positions, and passing it to voxel edits.

#### Scenario: Freezing from Python
- **WHEN** a script masks a region and then stamps a brush across it
- **THEN** the masked cells are unchanged

#### Scenario: Sampling returns an array
- **WHEN** a script samples the mask at an (N, 3) array of positions
- **THEN** it receives an (N,) array of values in [0, 1]
