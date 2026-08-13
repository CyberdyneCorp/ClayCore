# file-io Specification

## Purpose
TBD - created by archiving change add-claycore-v1. Update Purpose after archive.
## Requirements
### Requirement: Document format (.clayspace)
`clay::io` SHALL read and write the `.clayspace` single-file binary chunked container: versioned chunks for scene commands (the undo command vocabulary), palettes, voxel grids (palette+RLE compressed), thumbnails (PNG), and camera bookmarks. Readers SHALL open any older format version (backward-open) and SHALL refuse newer major versions with a clear error (forward-refuse), never crashing or partially loading. The format lives entirely in claycore so Python and CI read/write projects without the app.

#### Scenario: Round trip
- **WHEN** any golden-corpus document is saved and reloaded
- **THEN** the reloaded document evaluates bit-identically (same tapes, same brick results) and serializes to identical bytes

#### Scenario: Forward refusal
- **WHEN** a file with a higher major format version is opened
- **THEN** loading fails with a version-mismatch error code and no partial document is produced

### Requirement: OBJ + MTL
The module SHALL provide a dependency-free OBJ reader and writer with MTL, supporting the documented vertex-color extension on export.

#### Scenario: OBJ export with colors
- **WHEN** a colored mesh is exported to OBJ
- **THEN** vertex colors are written per the documented extension and the file reimports with colors intact

### Requirement: FBX
The module SHALL import FBX via ufbx (meshes, transforms, vertex colors) and export via a minimal binary FBX writer producing meshes, transforms, and vertex colors with correct units and axis conventions for Unity, Unreal, and Blender. CI SHALL validate exports by round-tripping through assimp and headless Blender.

#### Scenario: Engine-correct axes and units
- **WHEN** a 1-meter cube authored in claycore is exported to FBX and imported into Blender headless in CI
- **THEN** it measures 1 meter with +Z up handled per convention mapping (no 100× scale, no rotated axes)

### Requirement: PLY
The module SHALL read and write PLY with vertex colors (binary and ASCII), interoperable with the SDF Modeler / MagicaCSG ecosystems.

#### Scenario: PLY color round trip
- **WHEN** a colored mesh is written to binary PLY and read back
- **THEN** geometry and per-vertex colors are preserved exactly

### Requirement: glTF/GLB writer
The module SHALL write glTF 2.0 / GLB (via cgltf or a custom writer) with meshes, vertex colors, and node transforms, validating against the glTF validator in CI.

#### Scenario: Valid glTF
- **WHEN** a golden scene mesh is exported to GLB
- **THEN** the glTF validator reports zero errors

### Requirement: Import guardrails
All importers SHALL enforce triangle/vertex budgets (configurable), survive malformed-file fuzzing without crashes or unbounded allocation, and fail with error codes — never exceptions across the ABI.

Loaders SHALL validate declared counts against the actual payload size BEFORE allocating, and SHALL bound the memory a payload can decode into as well as the bytes it occupies. A run-length encoded payload SHALL be refused when its declared record count exceeds what the remaining bytes could describe.

#### Scenario: Malformed file rejected safely
- **WHEN** a fuzzed/truncated FBX, OBJ, or PLY file is imported
- **THEN** the importer returns an error code with bounded memory use (no crash, no allocation bomb)

#### Scenario: A run-length payload cannot claim more records than it has bytes
- **WHEN** a voxel or mask payload declares a chunk count larger than its remaining bytes could encode
- **THEN** it is refused before any chunk is allocated

#### Scenario: Declared counts still bound a well-formed file
- **WHEN** a file within the vertex and triangle budgets is loaded
- **THEN** it loads unchanged

### Requirement: USDZ exclusion
claycore SHALL NOT implement USDZ. It SHALL expose mesh + attribute buffers in a layout directly consumable by platform USD APIs (e.g. Apple Model I/O in the app shell).

#### Scenario: Buffers ready for Model I/O
- **WHEN** a consumer requests mesh buffers for platform export
- **THEN** positions, normals, colors, and indices are exposed as contiguous typed arrays with documented layout requiring no per-vertex conversion

### Requirement: The scene chunk carries a version
The scene payload SHALL be decoded against the container's minor version rather than assuming the current layout, so that a field added to a node does not require a packing trick to stay backward compatible. A document written at an earlier minor SHALL load with the new fields at their defaults.

#### Scenario: An older document loads with hard corners
- **WHEN** a document written before point types existed is loaded
- **THEN** every stroke point is a hard corner, no list is closed, and the field is what it always was

#### Scenario: Curves round trip
- **WHEN** a document containing a closed Bezier curve is saved and reloaded
- **THEN** the control points, their types, their handles, the closed flag and the tolerance all come back, and the field is unchanged

### Requirement: A path that is not a readable regular file is refused
Every `*_file` loader SHALL determine the length of a file before sizing a buffer from it, and SHALL refuse a path whose length cannot be established or exceeds the import budget's file ceiling. Opening succeeding is not evidence that a path is a file: a directory opens for reading on common platforms and reports a length that is not its own.

The refusal SHALL be an `IoStatus`, never a termination. The library builds without exceptions, so an allocation sized from a bogus length ends the host process rather than returning.

#### Scenario: A directory is refused
- **WHEN** any loader is given the path of a directory
- **THEN** it returns a failed `IoStatus` and the process continues

#### Scenario: A file above the ceiling is refused before it is read
- **WHEN** a file is longer than the import budget's `max_file_bytes`
- **THEN** the loader returns `BudgetExceeded` without allocating for its contents

#### Scenario: An ordinary file still loads
- **WHEN** a well-formed document or mesh file is loaded
- **THEN** it loads exactly as before

### Requirement: A PLY header that is not newline-terminated stays in bounds
The PLY reader SHALL treat the end of the buffer as the end of the header when no newline follows `end_header`, and SHALL NOT read, or compute a length from, any byte beyond the buffer it was given.

#### Scenario: A header truncated after end_header
- **WHEN** a PLY buffer ends immediately after `end_header` with no trailing newline
- **THEN** the reader returns without reading past the buffer and without terminating

### Requirement: Declared vertex counts are checked against the payload in both formats
The PLY reader SHALL check a declared vertex count against the bytes actually present for BOTH the binary and the ascii format, and SHALL refuse a vertex element that declares no properties, since a zero-width vertex makes any count fit any file.

#### Scenario: An ascii header over-declares
- **WHEN** an ascii PLY declares far more vertices than its payload can hold
- **THEN** the reader refuses it as `Malformed`

#### Scenario: A vertex element with no properties
- **WHEN** a PLY declares a vertex element carrying no properties and a non-zero count
- **THEN** the reader refuses it as `Malformed`

#### Scenario: A well-formed file need not end with a newline
- **WHEN** an ascii PLY whose final line carries no trailing newline is loaded
- **THEN** it loads, because the format does not require one and the payload floor must not be tighter than the format

### Requirement: An element the reader does not read is refused
The PLY reader SHALL refuse a file declaring a non-empty element it does not read, rather than ignoring the declaration. Only the declaration is skipped, not the bytes: the payload of an unread element stays in the stream and displaces the vertex data, so ignoring it returns a silently wrong mesh.

#### Scenario: An element declared before the vertices
- **WHEN** a PLY declares a non-empty element the reader does not understand
- **THEN** it is refused as `Unsupported`

### Requirement: A document's read ceiling is the caller's to raise
`load_clayspace_file` SHALL take an import budget, so the ceiling on what it reads into memory can be raised. Nothing caps what `save_clayspace_file` writes, and a document carrying sampled volumes is large by nature, so a fixed reader ceiling would make a document this library had just written permanently unopenable.

#### Scenario: A budget below the file size refuses it
- **WHEN** a document is loaded with a `max_file_bytes` smaller than the file
- **THEN** it returns `BudgetExceeded`

#### Scenario: The same document loads under the default
- **WHEN** the same document is loaded with the default budget
- **THEN** it loads

### Requirement: An imported mesh satisfies the mesh invariant
A loader SHALL NOT return a mesh whose normals, colors or uvs array is non-empty and of a different length than its positions array. Where a source file supplies an attribute for only some of its objects, the loader SHALL drop that attribute rather than return a short one.

#### Scenario: An FBX where only one object is painted
- **WHEN** an FBX carrying two meshes, only one with a color layer, is imported
- **THEN** the resulting mesh's colors array is either empty or exactly as long as its positions array

### Requirement: The node record carries a tree
The node record SHALL carry an armature's parent indices alongside its points, gated on the minor so that a reader predating armatures is unaffected, and its signs the same way at the following minor, so that a reader predating signs is unaffected by an all-positive document written at its own minor.

Writing at a minor below the signs minor SHALL drop the signs and reproduce the older bytes exactly — the existing escape hatch for an older build — and a document whose armature is all-positive SHALL lose nothing to it.

#### Scenario: An older reader is not broken by an armature
- **WHEN** a reader that predates armatures opens a document containing one
- **THEN** it opens the document rather than refusing it, and the armature is absent rather than corrupt

#### Scenario: Signs round trip at the current minor
- **WHEN** a document holding an armature with a negative node is saved and reloaded at the current minor
- **THEN** the signs read back exactly and the document reserialises to identical bytes

#### Scenario: Writing at the previous minor drops only the signs
- **WHEN** the same document is written at the minor below the signs minor
- **THEN** the bytes are exactly what that minor produced before signs existed, and reloading them yields the all-positive rig

