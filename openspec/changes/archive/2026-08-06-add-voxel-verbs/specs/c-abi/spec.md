# c-abi — the remaining voxel verbs

Delta for `add-voxel-verbs`.

## ADDED Requirements

### Requirement: The new verbs across the ABI
The C API SHALL expose the four verbs, with the alpha as a packed float array plus its width and height.

#### Scenario: A verb means the same through both bindings
- **WHEN** a C consumer runs each verb with given parameters
- **THEN** the grid matches what `pyclay` produces for the same call
