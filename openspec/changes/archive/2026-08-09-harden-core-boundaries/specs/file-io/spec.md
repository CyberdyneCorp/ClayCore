# file-io — refusals at the import boundary

Delta for `harden-core-boundaries`.

## ADDED Requirements

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

## MODIFIED Requirements

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
