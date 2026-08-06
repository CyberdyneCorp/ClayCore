# c-abi — repair

Delta for `add-voxel-repair`.

## ADDED Requirements

### Requirement: Repair across the ABI
The C API SHALL expose the report through a versioned descriptor and both repairs, each taking an optional mask.

#### Scenario: A repair means the same through both bindings
- **WHEN** a C consumer repairs a grid
- **THEN** the result matches what `pyclay` produces for the same call
