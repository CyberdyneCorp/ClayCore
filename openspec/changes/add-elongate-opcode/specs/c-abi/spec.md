# c-abi — elongate

Delta for `add-elongate-opcode`.

## ADDED Requirements

### Requirement: elongate across the ABI
`clay_deform` SHALL include an elongate enumerator taking the three half-extents, so a C consumer composes the same stretched item the Python bindings do.

#### Scenario: Stretching from C
- **WHEN** a C consumer appends an elongate deformer to an item builder
- **THEN** the document evaluates identically to the same item authored through the scene API
