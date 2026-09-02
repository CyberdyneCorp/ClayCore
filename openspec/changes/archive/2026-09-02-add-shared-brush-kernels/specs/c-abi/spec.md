# c-abi — brush presets

Delta for `add-shared-brush-kernels`.

## ADDED Requirements

### Requirement: A host can carry a brush preset across the ABI
The C ABI SHALL expose the brush preset — the stroke preset it contains and the model axes the mesh path already honours — through versioned descriptors following the established `struct_size` pattern, with bounded output fills.

Serialization SHALL cross as bytes rather than as a path, matching every other format the library writes, so a host holding a preset library in its own container never writes a temporary file.

Image content SHALL remain borrowed for the duration of a call. The ABI SHALL NOT take ownership of alpha or displacement samples, and SHALL NOT copy them into a preset.

Existing mesh brush entry points SHALL keep their semantics unchanged. A host compiled against the current header SHALL build and behave identically after this change.

#### Scenario: A preset crosses and comes back
- **WHEN** a preset is serialized through the ABI, deserialized, and used to resolve a stroke
- **THEN** the resolved stamps equal those from the original preset

#### Scenario: An older descriptor is honoured
- **WHEN** a host passes a descriptor whose `struct_size` predates a field added later
- **THEN** the call succeeds using defaults for the fields it does not carry, and writes no byte past the size the caller declared
