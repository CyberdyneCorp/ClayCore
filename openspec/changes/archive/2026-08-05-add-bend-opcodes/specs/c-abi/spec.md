# c-abi — the ramped bends

Delta for `add-bend-opcodes`.

## ADDED Requirements

### Requirement: bend_linear and bend_radial across the ABI
`clay_deform` SHALL include enumerators for both ramped bends, taking nine and three parameters respectively, so a C consumer composes the same item the Python bindings do.

#### Scenario: Ramping from C
- **WHEN** a C consumer appends either bend to an item builder
- **THEN** the document evaluates identically to the same item authored through the scene API

#### Scenario: A degenerate span is refused
- **WHEN** either bend is given a zero-length span
- **THEN** the call returns `CLAY_ERROR_INVALID_ARGUMENT`
