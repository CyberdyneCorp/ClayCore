# file-io — persisting a hierarchy

Delta for `add-mesh-multires`.

## ADDED Requirements

### Requirement: A multiresolution surface has its own versioned encoding
A multiresolution surface SHALL serialize through its own versioned encoding carrying a magic, a version, the subdivision rule it was built with, validated counts, overflow-checked sizes and the detail channels present.

The subdivision rule SHALL be recorded rather than assumed. A hierarchy reconstructed with a different rule than it was authored with is a different surface, and nothing else in the stream reveals the substitution.

The document format SHALL remain UNCHANGED. A hierarchy is a standalone handle no layer owns, so its encoding is a blob a host stores beside the document — the shape an adaptive surface's encoding already has — and the `.clayspace` format gains no chunk. A document written after this change is therefore byte-identical to one written before it, which is a stronger guarantee than the backward-open one this requirement originally asked for and is available for the same reason the memory accounting is per surface.

A decoder SHALL reject counts and depths whose reconstruction would exceed its own ceiling BEFORE allocating, as the voxel level tail already requires — a few hundred bytes declaring a deep hierarchy over a large base is a request for more memory than a machine holds.

#### Scenario: A hierarchy round-trips
- **WHEN** a hierarchy with detail at several levels is encoded and decoded
- **THEN** the cage, the rule, every level's detail and the active levels are restored exactly, and the surface it reconstructs is bit-identical

#### Scenario: A hostile depth is refused before allocation
- **WHEN** a stream declares a depth whose subdivision of its base would exceed the reader's ceiling
- **THEN** the load fails with a typed error and allocates nothing
