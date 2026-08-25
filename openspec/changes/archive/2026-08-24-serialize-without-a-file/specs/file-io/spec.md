# file-io

## ADDED Requirements

### Requirement: Every format is reachable without a filesystem
Each format the module reads or writes SHALL be reachable by a consumer of the library as bytes, not only as a path.

The module already implements every format against buffers and implements the path forms as wrappers over them. Wrapping SHALL NOT be the only form a consumer can reach: a host whose documents arrive from a document provider, a network, a pasteboard or its own container has no path to give, and requiring one forces a temporary file whose cost, cleanup and failure modes have nothing to do with what the host asked for.

The path forms SHALL remain, SHALL keep their behaviour, and SHALL be defined as the wrappers they already are, so that there is one implementation of each format rather than two that could drift.

An in-memory OBJ SHALL carry no `mtllib` reference. The path form writes a companion `.mtl` beside the object file and names it; a buffer has no companion, and naming one that does not exist would be worse than naming none.

#### Scenario: A format is reachable both ways
- **WHEN** a consumer saves a mesh to memory in each supported format and loads each buffer back
- **THEN** every format round-trips, and each buffer is identical to what the path form writes

#### Scenario: An in-memory OBJ names no material file
- **WHEN** a mesh is saved to memory as OBJ
- **THEN** the text contains no `mtllib` line, rather than one naming a file that was never written
