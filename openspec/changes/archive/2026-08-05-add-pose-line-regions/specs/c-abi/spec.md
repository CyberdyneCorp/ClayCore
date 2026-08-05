# c-abi — pose_line

Delta for `add-pose-line-regions`.

## ADDED Requirements

### Requirement: pose_line across the ABI
`clay_deform` SHALL include a line-gradient pose enumerator taking anchor, end, axis and angle.

#### Scenario: Posing from C
- **WHEN** a C consumer appends a line pose to an item builder
- **THEN** the document evaluates identically to the same item authored through the scene API

#### Scenario: A degenerate segment is refused
- **WHEN** the anchor and end coincide
- **THEN** the call returns `CLAY_ERROR_INVALID_ARGUMENT`
