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

### Requirement: OBJ, PLY and FBX write quads when a mesh carries them
The OBJ, PLY and FBX writers SHALL write a quad mesh as quads: OBJ as `f a b c d` with the same corner spelling the triangle writer uses for positions, uvs and normals; PLY with `element face` counting quads and each row written as a four-index list; FBX as four indices per polygon in `PolygonVertexIndex`, the last one's complement marking the end as it already does for three.

A mesh with no quads SHALL be written exactly as it is written today, byte for byte, in all three formats. Nothing about the triangle path changes.

The readers SHALL NOT change. OBJ and PLY keep fan-triangulating the faces they read, so a quad file re-imported comes back as triangles. This asymmetry SHALL be stated in the header rather than discovered: preserving faces on import is a second direction with its own budget and validation questions, and the readers already hold the face list, so it stays a cheap follow-up rather than a hidden gap.

#### Scenario: A quad mesh exports as quads
- **WHEN** a quad mesh is written to OBJ, to PLY and to FBX
- **THEN** each file declares one four-corner face per quad, with no triangles, and the corner order matches the mesh's quad array

#### Scenario: A triangle mesh exports as it always did
- **WHEN** any mesh carrying no quads is written to OBJ, PLY, FBX or GLB
- **THEN** the bytes are identical to those the writer produced before quads existed

#### Scenario: A quad file re-imports as triangles
- **WHEN** an exported quad OBJ or PLY is read back
- **THEN** it loads as the fan triangulation of its faces, carrying no quads, and the header states this

### Requirement: glTF stays triangles because glTF has no quads
The glTF/GLB writer SHALL keep writing the triangulation, unchanged, for quad meshes and triangle meshes alike. glTF 2.0 defines no quad primitive mode, so a conforming file cannot carry one.

This SHALL be stated where a caller will meet it — in the mesh I/O header and beside the C ABI's save entry point — rather than left to be discovered by exporting a quad mesh to GLB and opening it. "I exported GLB and got triangles" is the most likely report this feature can generate, and it is not a defect.

#### Scenario: A quad mesh to GLB is triangles
- **WHEN** a quad mesh is written to GLB
- **THEN** the primitive is mode 4 with the mesh's triangle indices, the file validates, and the headers state that glTF carries no quads

### Requirement: The document's mesh stream carries quads without refusing older readers
The `.clayspace` mesh stream SHALL carry a mesh's quads as a section APPENDED after the triangle indices — a count followed by four indices per quad — so a mesh layer holding a quad mesh keeps it across a save and load.

It SHALL NOT be signalled by a new attribute-mask bit and SHALL NOT move the format version. The reader already bounds the declared geometry against the bytes present rather than requiring equality, precisely so a later minor may append a section an older reader skips. An unknown mask bit, by contrast, is refused outright, and the format's minors are forward-refused — either would make an older build reject an entire document rather than miss an optional section whose information is already present as triangles.

A quad section that is PRESENT but malformed — a count that does not fit the bytes remaining, an index past the vertex array, or a quad list that is not the triangulation of the triangles in the same chunk — SHALL refuse the stream as malformed. That is what the reader already does with an out-of-range triangle index: a chunk this library did not write is not trusted to be half right.

A mesh with no quads SHALL serialise to exactly the bytes it serialises to today.

The WRITER SHALL NOT emit a quad section the reader would refuse: a mesh whose quad list is not the triangulation beside it serialises as the triangles it carries, without the section. Writing it would produce a document this library refuses to open, which is a worse failure than losing an optional array that was already describing triangles that do not exist.

Because the reader claims the FIRST tail after the indices, a later section appended to this stream SHALL be written after the quad section rather than before it; bytes past the quad list are skipped, as bytes past the indices were.

#### Scenario: An inconsistent quad list is written as triangles, not as a refusable document
- **WHEN** a mesh whose quad list does not match its triangles is written to the stream
- **THEN** the bytes carry no quad section, and they load as the mesh's triangles

#### Scenario: A quad mesh layer survives a document round trip
- **WHEN** a document holding a quad mesh layer is saved and loaded
- **THEN** the mesh reads back with its quads, its triangles and its attributes unchanged

#### Scenario: An older reader opens the document and sees triangles
- **WHEN** a document containing a quad mesh is read by a build that predates the quad section
- **THEN** the document opens, the mesh reads as its triangles, and nothing is refused

#### Scenario: A corrupt quad section is refused
- **WHEN** a mesh chunk carries a quad section whose indices do not match the triangles present
- **THEN** loading fails as malformed rather than returning a mesh whose quad array contradicts its triangles

#### Scenario: A triangle mesh's bytes do not move
- **WHEN** a mesh carrying no quads is written to the stream
- **THEN** the bytes are identical to those written before the quad section existed

### Requirement: A volume's colour is written and read back
A serialised volume SHALL carry its colour section when it has one, and SHALL record its absence when it does not. A colour a document cannot save is a colour a sculptor loses on reload, which is the failure this whole change exists to remove one level up.

The section SHALL be absent-or-present as a whole, with its own length, so an uncoloured volume costs a marker rather than an empty array.

The document format minor SHALL move to 9, and the container and scene payload versions SHALL move together as the existing static assertion requires.

A document written at minor 8 SHALL open: its volumes have no colour section and SHALL read as uncoloured, which is what they are. A document written at minor 9 SHALL be REFUSED by an older reader under the existing forward-refuse rule, rather than being read with a corrupt tail.

#### Scenario: Colour survives a round trip through a file
- **WHEN** a document containing a coloured volume is saved and loaded
- **THEN** the volume's per-sample colours are what they were, and evaluating the document reports the same colours

#### Scenario: An older document still opens
- **WHEN** a minor-8 document containing a volume is loaded by this build
- **THEN** it opens, the volume reads as uncoloured, and evaluation reports the item's constant colour as it always did

#### Scenario: A newer document is refused rather than misread
- **WHEN** a minor-9 document is opened by a reader built before this change
- **THEN** it is refused on version grounds

### Requirement: A voxel layer's finer levels are stored as offsets
A voxel grid's stream SHALL open with the COARSEST level in the layout it already had, and any further level SHALL follow as a tagged tail carrying only that level's per-cell offsets — the cells whose value differs from the cell above them.

Only the offsets are stored because everything else is reproducible by subdividing, so a level that carries no detail costs a count and nothing else. Storing every level in full would multiply a document's size by eight per level for content that is derivable, which is the size question the proposal left open.

A grid with a single level SHALL write no tail at all, so its bytes are exactly the bytes it wrote before levels existed.

#### Scenario: A one-level grid is byte-identical
- **WHEN** a grid that was never given a second level is serialised
- **THEN** the bytes are identical to those the same grid produced before levels existed

#### Scenario: A stack round trips
- **WHEN** a grid with several levels is saved and reloaded
- **THEN** every level holds the same cells, the active level is the one that was saved, and saving again produces identical bytes

#### Scenario: A malformed tail is refused
- **WHEN** a stream's level tail is truncated, names an impossible level count, or carries a palette index the file does not hold
- **THEN** the grid is refused as malformed rather than loaded partially built

### Requirement: A tail may not cost more than the file pays for
The reader SHALL charge a tail's declared depth against the content the file actually supplied, and SHALL refuse one whose levels it would have to materialise beyond a fixed ceiling, before building any of them.

Storing only the offsets means a tail stays small however deep the stack it names, and every level above the coarsest is rebuilt by subdividing — so a fixed-size tail asks for eight times the cells per level it declares. A depth limit alone does not bound that: a few hundred bytes claiming the maximum depth over a modest coarsest level is a request for more cells than a machine holds. The check is exact rather than an estimate, because subdivision is exact.

#### Scenario: A tiny file cannot ask for an unbounded grid
- **WHEN** a stream's tail declares a depth whose subdivision of the coarsest level would exceed the reader's ceiling
- **THEN** the grid is refused immediately, without allocating any of the levels it named

#### Scenario: A stack the file pays for still opens
- **WHEN** a stream's declared depth is within what the coarsest level's content justifies
- **THEN** it loads normally, so the guard refuses the malformed case and not the format

### Requirement: A reader that predates levels opens the document at the coarsest level
The container's major version SHALL NOT change, and no new chunk type is introduced: the tail lives inside the existing voxel chunk, after the point at which a reader written before levels stops. That reader SHALL open the document, read the coarsest level, and ignore the tail — it SHALL NOT fail, and it SHALL NOT misread the tail as chunk data.

The container minor and the scene minor SHALL both advance, bound by the static assertion that already keeps them together, because the container's content changed even though the scene payload did not. A reader that predates the tail SHALL lose the finer levels if it saves the document again, and the format notes SHALL say so.

#### Scenario: An older reader opens a newer document
- **WHEN** a document whose voxel layers carry several levels is opened by a reader written against the previous minor
- **THEN** it opens with each voxel layer at its coarsest level, and nothing else in the document is affected

#### Scenario: A newer reader opens an older document
- **WHEN** a document written before levels existed is loaded
- **THEN** every voxel layer has exactly one level and is otherwise exactly what it was

