# c-abi — sculpting verbs for an SDF layer

Delta for `add-sdf-sculpt-verbs`.

## ADDED Requirements

### Requirement: Shaping verbs across the ABI
The C API SHALL expose the shaping verbs on a layer, taking a centre, a radius, the verb's own parameters and a falloff, and SHALL be purely additive: no existing signature changes and no struct grows.

A verb that is valid but has no effect SHALL report success, and the count of what it changed SHALL be discoverable rather than inferred — the same rule `clay_voxel_change_count` establishes for the voxel verbs.

#### Scenario: A verb means the same through both bindings
- **WHEN** a C consumer applies a shaping verb with given parameters
- **THEN** the field matches what `pyclay` produces for the same call
