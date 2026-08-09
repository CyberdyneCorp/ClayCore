# c-abi — sculpt layers

Delta for `add-sculpt-layers`.

## ADDED Requirements

### Requirement: Sculpt layers across the ABI
The C API SHALL expose recording a pass, setting its strength and visibility, reordering, deleting and merging down, and SHALL be purely additive.

#### Scenario: A document without sculpt layers is unaffected
- **WHEN** a caller never records a pass
- **THEN** every existing call behaves exactly as before
