# c-abi — the host seam at scale

Delta for `add-extreme-poly-runtime`.

## ADDED Requirements

### Requirement: A host updates what changed, with buffers it owns
The C ABI SHALL expose changed surface chunks with their revisions, and SHALL let a caller query capacity and supply its own buffers for positions, normals and indices.

The ABI SHALL NOT allocate a heap object per changed chunk per frame, and SHALL NOT require a host to copy a whole surface to observe a stamp.

A readback SHALL carry the revision it describes, so a host can discard a result that a later mutation has superseded.

An acknowledgement call SHALL let a host drain incrementally across frames without losing a change it has not yet applied.

#### Scenario: A stroke's transport follows the change
- **WHEN** a host drains changed chunks each frame during a stroke on a large surface
- **THEN** the bytes copied follow the changed chunks rather than the surface size

#### Scenario: A stale readback is identifiable
- **WHEN** a host applies a readback taken before a further mutation
- **THEN** the revisions it carries let the host detect that it is stale

### Requirement: A host sets a memory profile and asks for a trim
The C ABI SHALL accept a memory profile descriptor, SHALL expose a trim call taking a pressure level and returning what was released, and SHALL report runtime memory by the same categories the document report uses.

Descriptors SHALL follow the established `struct_size` pattern with bounded output fills.

The ABI SHALL NOT expose an entry point that releases authoritative content as part of a trim.

#### Scenario: A trim under pressure preserves the document
- **WHEN** a host sets a constrained profile and requests a critical trim
- **THEN** the call reports the released caches and the document's authoritative checksum is unchanged
