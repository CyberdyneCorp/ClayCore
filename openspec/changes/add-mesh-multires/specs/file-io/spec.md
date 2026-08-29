# file-io — persisting a hierarchy

Delta for `add-mesh-multires`.

## ADDED Requirements

### Requirement: A multiresolution surface has its own versioned encoding
A multiresolution surface SHALL serialize through its own versioned encoding carrying a magic, a version, the subdivision rule it was built with, validated counts, overflow-checked sizes and the detail channels present.

The subdivision rule SHALL be recorded rather than assumed. A hierarchy reconstructed with a different rule than it was authored with is a different surface, and nothing else in the stream reveals the substitution.

The document format SHALL remain BACKWARD-OPEN: a reader predating this chunk SHALL open the document without the hierarchy rather than failing, and a document holding no hierarchy SHALL be byte-identical to one written before this change.

A decoder SHALL reject counts and depths whose reconstruction would exceed its own ceiling BEFORE allocating, as the voxel level tail already requires — a few hundred bytes declaring a deep hierarchy over a large base is a request for more memory than a machine holds.

#### Scenario: A hierarchy round-trips
- **WHEN** a document holding a hierarchy with detail at several levels is saved and reloaded
- **THEN** the base, the rule, every level's detail and the active level are restored exactly

#### Scenario: A hostile depth is refused before allocation
- **WHEN** a stream declares a depth whose subdivision of its base would exceed the reader's ceiling
- **THEN** the load fails with a typed error and allocates nothing
