# c-abi — grab and pose

Delta for `add-region-deformers`.

## ADDED Requirements

### Requirement: grab and pose across the ABI
`clay_deform` SHALL include grab and pose enumerators, and the voxel surface SHALL gain a grab entry point, so a Swift consumer drives the same tools the Python bindings do.

#### Scenario: Grabbing from C
- **WHEN** a C consumer appends a grab deformer to an item builder
- **THEN** the document evaluates identically to the same item authored through the scene API

#### Scenario: A non-positive radius is refused
- **WHEN** either entry point is given a radius of zero
- **THEN** it returns `CLAY_ERROR_INVALID_ARGUMENT`
