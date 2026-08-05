# c-abi — elongate_axis

Delta for `add-elongate-axis-opcode`.

## ADDED Requirements

### Requirement: elongate_axis across the ABI
`clay_deform` SHALL include a per-axis elongate enumerator taking the three half-extents.

#### Scenario: Stretching from C
- **WHEN** a C consumer appends a per-axis elongate to an item builder
- **THEN** the document evaluates identically to the same item authored through the scene API
