# file-io

## ADDED Requirements

### Requirement: GLB import
The library SHALL READ glTF 2.0 binary (`.glb`) files, not only write them. Every other mesh format the library writes it also reads, and GLB is the one a host is most likely to present first.

The reader SHALL import every mesh and every `TRIANGLES` primitive in the file, concatenated into one mesh, with each node's world transform applied to positions and the inverse transpose applied to normals. A file whose geometry is placed by a node hierarchy SHALL arrive as the shape its author saw rather than as pieces at the origin.

It SHALL accept the accessor forms real exporters emit and not only those this library writes: `uint8`, `uint16` and `uint32` indices, non-indexed primitives, interleaved bufferViews carrying a `byteStride`, and `COLOR_0` as `VEC3` or `VEC4` in float, `uint8` or `uint16` with the specification's normalization.

Data the mesh type cannot carry — materials, textures, animation, skinning, cameras, morph targets — SHALL be IGNORED so that an asset carrying it still imports its geometry. A primitive whose mode is not `TRIANGLES` SHALL be REFUSED rather than skipped, because importing a line or point set as an empty mesh is indistinguishable from a broken reader.

`.gltf` SHALL NOT be accepted by a path-taking loader: its buffers are separate files, and resolving them would mean reading files the caller never supplied.

Because a mesh file is UNTRUSTED input, the reader SHALL validate before it reads: every accessor SHALL be bounds-checked against its bufferView and against the BIN chunk before any element is fetched, the GLB's declared total length SHALL NOT be trusted over the actual byte count, JSON nesting depth SHALL be bounded, and the node walk SHALL terminate on a self-referencing hierarchy. Declared counts SHALL be checked against the import budget before allocation, as every other importer already does.

#### Scenario: A written file reads back
- **WHEN** a mesh is saved as `.glb` and loaded again
- **THEN** positions, indices and every present attribute come back bit-identical

#### Scenario: Another exporter's file
- **WHEN** a GLB using interleaved bufferViews, `uint16` indices, normalized `uint8` colours and a node transform is loaded
- **THEN** the geometry arrives transformed into world space with its colours decoded

#### Scenario: A file that lies about itself
- **WHEN** a GLB is truncated, declares a chunk longer than the file, carries malformed JSON, or contains an accessor or index reaching past its data
- **THEN** the load is refused with a diagnostic and no out-of-bounds read occurs

#### Scenario: Geometry that cannot be represented
- **WHEN** a primitive declares a mode other than `TRIANGLES`
- **THEN** the load is refused naming the mode, rather than returning a mesh missing that primitive
