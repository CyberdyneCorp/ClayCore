# c-abi — wrap_around

Delta for `add-wrap-around-opcode`.

## ADDED Requirements

### Requirement: wrap_around across the ABI
`clay_deform` SHALL include a wrap enumerator taking `x0` and `x1`, so a C consumer composes the same wrapped item the Python bindings do.

#### Scenario: Wrapping from C
- **WHEN** a C consumer appends a wrap deformer to an item builder
- **THEN** the document evaluates identically to the same item authored through the scene API
