# c-abi — loft

Delta for `add-loft-opcode`.

## ADDED Requirements

### Requirement: Lofts across the ABI
The C API SHALL expose adding profiles to a loft item, including polygon profiles with their vertices.

#### Scenario: A loft means the same through both bindings
- **WHEN** a C consumer builds a loft of the same profiles
- **THEN** the field matches what `pyclay` produces
