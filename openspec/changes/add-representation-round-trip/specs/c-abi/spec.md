# c-abi — move a sculpt between the two representations

Delta for `add-representation-round-trip`.

## ADDED Requirements

### Requirement: Conversion across the ABI
The C API SHALL expose both directions, taking the cell size and band width for the field direction, and SHALL be purely additive.

#### Scenario: Both bindings agree
- **WHEN** the same conversion runs through the C ABI and through pyclay
- **THEN** the results evaluate identically
