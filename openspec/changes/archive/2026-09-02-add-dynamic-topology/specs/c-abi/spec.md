# c-abi — adaptive surfaces

Delta for `add-dynamic-topology`.

## ADDED Requirements

### Requirement: A host can sculpt an adaptive surface across the ABI
The C ABI SHALL expose the adaptive surface and its sculptor as opaque handles, with versioned descriptors for the surface, the topology policy and the stamp report, following the established `struct_size` pattern with bounded output fills.

`clay_mesh_sculptor` SHALL keep its semantics unchanged. A host compiled against the current header relies on stable vertex and index counts and on borrowed buffers; adaptive topology SHALL NOT reach it.

The ABI SHALL report topology, geometry and attribute revisions separately, and SHALL expose the changed partitions of a stroke with caller-owned buffers and a capacity query rather than copying the whole surface per stamp.

Long operations — construction, global remesh, conversion, serialization — SHALL accept the cancellation token, and a cancelled operation SHALL leave the surface byte-identical.

#### Scenario: The fixed sculptor is unchanged
- **WHEN** a host built against the previous header calls the fixed mesh sculptor after this change
- **THEN** it behaves identically, and no adaptive behaviour reaches it

#### Scenario: A stroke updates only what changed
- **WHEN** a host drives a stroke and drains the changed partitions each frame
- **THEN** the bytes it copies follow the changed partitions rather than the size of the surface

#### Scenario: A cancelled build changes nothing
- **WHEN** a long adaptive operation is cancelled through the token
- **THEN** the call reports cancellation and the surface is byte-identical to before it started
