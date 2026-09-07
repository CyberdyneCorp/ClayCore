# file-io Specification

## Purpose
Getting a model in and out, and being explicit about what survives the trip.

The `.clayspace` container is the one format that round-trips a document
losslessly, and it is BACKWARD-OPEN by rule: an older reader opens a newer file
without the parts it does not know rather than failing. The interchange formats
— OBJ, PLY, FBX, glTF — carry geometry out to other tools and carry meshes in,
under import guardrails, because an imported file is the one input this library
does not control.
## Requirements
### Requirement: Document format (.clayspace)
`clay::io` SHALL read and write the `.clayspace` single-file binary chunked container: versioned chunks for scene commands (the undo command vocabulary), palettes, voxel grids (palette+RLE compressed), imported meshes, thumbnails (PNG), and camera bookmarks. Readers SHALL open any older format version (backward-open) and SHALL refuse newer major versions with a clear error (forward-refuse), never crashing or partially loading. The format lives entirely in claycore so Python and CI read/write projects without the app.

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

### Requirement: A mesh can be written as a sculpt handoff
The library SHALL write a mesh in the sculpt handoff profile that CyberRemesherAndUV's reader accepts, so that a sculpt can enter a retopology, UV and bake pipeline without either engine linking the other.

The handoff SHALL be available as a FILE and as IN-MEMORY BUFFERS, since both engines may run in one process — on a tablet especially, where writing a file to hand a mesh to a library in the same address space is not a reasonable step.

The writer SHALL declare the handoff version and MAY declare a producer label, since a reader that cannot tell a handoff from an ordinary mesh of the same format has no way to apply the version gate.

#### Scenario: A written handoff declares its version
- **WHEN** a mesh is written as a handoff
- **THEN** the result declares the handoff version, and a reader can distinguish it from an ordinary mesh file of the same format

#### Scenario: Every required payload is present
- **WHEN** a mesh is written as a handoff
- **THEN** positions, per-vertex normals, per-vertex colours and a per-vertex material mix are all present

### Requirement: A handoff is always triangulated and always carries normals
The handoff writer SHALL write the triangle list as the faces, even for a mesh that also carries quads, and SHALL write per-vertex normals whether or not the mesh already had them.

Both are guarantees of the WRITER rather than requirements on the caller, because both are conditions the reader enforces and a caller cannot be expected to know. A mesh produced by quad export carries quads and is the export most likely to be handed over; a mesh meshed without gradients carries no normals. Handing either to the reader unchanged produces a rejected file for a reason the caller did nothing to cause.

Computing normals SHALL NOT modify the mesh being written.

#### Scenario: A quad mesh is handed over as triangles
- **WHEN** a mesh carrying quads is written as a handoff
- **THEN** the faces written are triangles, and the surface they describe is the same one the quads described

#### Scenario: A mesh without normals gains them
- **WHEN** a mesh with no normals is written as a handoff
- **THEN** the handoff carries per-vertex normals, and the source mesh is unchanged

### Requirement: The material mix comes from a mask, or is zero
The handoff's per-vertex material mix SHALL be derived from a MASK when the caller names one, by resolving the mask at each vertex position, and SHALL be zero for every vertex otherwise.

A mask is already a painted scalar in `[0, 1]` resolvable at any point, which is the shape and the meaning the handoff asks of this channel. The library SHALL NOT introduce material slots to satisfy it: a document that never expressed a material mix has none, and zero is the honest answer rather than an invented one.

#### Scenario: A masked region reports its mix
- **WHEN** a handoff is written with a mask that covers part of the mesh
- **THEN** vertices inside the painted region carry the mask's value and vertices outside it carry zero

#### Scenario: No mask means no mix
- **WHEN** a handoff is written without a mask
- **THEN** the material mix is zero for every vertex, and the payload is still present

### Requirement: A layer record may reference another layer's content
From scene minor 15 a layer record SHALL carry a content-source layer id: 0 meaning the layer owns the content that follows in the record, any other id meaning the layer shares the content of the layer with that id and carries none of its own.

Ownership SHALL be derived at write time from the identity of the content itself, in STACK ORDER: the first layer holding a given edit list owns it and every later holder names it. No flag is stored for it, so the file cannot disagree with the document, and a document whose original source layer was removed while its instances remain writes correctly with no special case.

A reader SHALL resolve the names in a second pass, after every layer record is read, and SHALL REFUSE a document naming a source it does not have rather than open it with an empty or duplicated edit list.

Writing at minor 14 or below SHALL write every layer's content inline as it always did. Such a document opens in an older build with the instances as INDEPENDENT COPIES: the shapes are right and the share is gone, so an edit through one no longer reaches the others. That is the recoverable direction, and it is why the writer takes a minor.

A build that predates minor 15 reading a minor 15 document SHALL fail rather than misread, which is the layer record's existing trade: the record is not length-prefixed, so a field it does not expect desynchronises every record after it and the reader's bounds and count checks reject the stream.

#### Scenario: Shared content is written once
- **WHEN** a document with a source layer and nine instances of it is saved
- **THEN** the edit list appears once in the file and the nine instances carry a reference to it

#### Scenario: A reload restores the sharing
- **WHEN** such a document is loaded
- **THEN** the ten layers hold one edit list and an edit through any of them is visible through all

#### Scenario: A removed source still writes
- **WHEN** the layer that was originally instanced is removed and the document is saved and reloaded
- **THEN** the surviving instances hold one edit list, owned by the first of them in stack order

#### Scenario: Writing at an older minor drops only the sharing
- **WHEN** a document with an instance is written at minor 14
- **THEN** each layer carries its own copy of the content, and both evaluate as they did

#### Scenario: A reference to a layer that is not in the file is refused
- **WHEN** a document names a content source it does not contain
- **THEN** the load fails

### Requirement: An adaptive surface has its own versioned encoding
An adaptive surface SHALL serialize through its own versioned encoding carrying a magic, a version, validated counts, overflow-checked sizes and the attribute channels present. It SHALL NOT be written into the existing mesh stream, whose readers expect flat interchange arrays.

A decoder SHALL reject hostile or truncated counts BEFORE allocating, following the defensive style the sparse vertex delta decoder already uses.

The document format SHALL remain BACKWARD-OPEN: a reader that predates this chunk SHALL open the document without it rather than failing, and a document containing no adaptive surface SHALL be byte-identical to one written before this change.

A round trip SHALL restore the surface exactly. Where an identity that cannot survive a round trip is not preserved — a generation counter, say — the format SHALL state that rather than imply preservation it does not provide.

#### Scenario: A round trip preserves the surface
- **WHEN** a document holding an adaptive surface is saved and reloaded
- **THEN** the surface's geometry, connectivity and attributes are restored exactly

#### Scenario: An older reader is not broken
- **WHEN** a reader that predates this chunk opens a document containing one
- **THEN** it opens the document without the adaptive surface rather than reporting a corrupt file

#### Scenario: A truncated stream is refused
- **WHEN** a stream declares counts larger than its own remaining bytes
- **THEN** the decode fails before allocating and reports a typed error

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

### Requirement: A mesh layer's geometry is stored in the document
The `.clayspace` container SHALL carry a mesh chunk per mesh layer, keyed by layer id, holding the decoded triangles rather than a reference to the file they came from. A reference would make the document's bytes depend on a file outside the container and on the importer's version, so the same document would yield different geometry after that file was edited and would fail to open once it was gone — which is not a container the round-trip requirement can be stated over.

The chunk SHALL declare its vertex count, its index count and which of normals, colors and uvs are present, and SHALL store the arrays uncompressed. Geometry SHALL be written exactly as it is held, so the round trip is an identity rather than a re-derivation.

The source path and the import parameters MAY be recorded as advisory provenance. They SHALL NOT be consulted when loading, and a document whose recorded path no longer resolves SHALL load unaffected.

#### Scenario: A mesh round trips byte for byte
- **WHEN** a document containing mesh layers is saved and reloaded
- **THEN** every mesh's positions, normals, colors, uvs and indices are identical, and saving again produces identical bytes

#### Scenario: The source file is not needed
- **WHEN** a document is reloaded after the file its mesh was imported from has been deleted or edited
- **THEN** it loads with the geometry it was saved with

#### Scenario: A mesh with no attributes stays that way
- **WHEN** a mesh carrying only positions and indices is saved and reloaded
- **THEN** its normals, colors and uvs are still empty rather than filled in with defaults

### Requirement: A mesh chunk's declared counts are checked before anything is allocated
The mesh reader SHALL validate a chunk against the bytes actually present before allocating for it: the vertex count SHALL be bounded by what the remaining bytes could hold given the attributes the chunk declares, the index count SHALL be bounded by the remaining bytes and SHALL be a multiple of three, and every index SHALL be less than the vertex count.

The index bound is not optional. A document's meshes are handed to a host as borrowed contiguous buffers, so an index outside the vertex array in a file the library did not write becomes an out-of-bounds read in the host.

A chunk that fails any of these checks SHALL be refused as malformed, and the library builds without exceptions, so the refusal SHALL be a status rather than a termination.

#### Scenario: An over-declared vertex count is refused
- **WHEN** a mesh chunk declares more vertices than its remaining bytes could hold
- **THEN** it is refused before any array is allocated

#### Scenario: An index outside the vertex array is refused
- **WHEN** a mesh chunk carries an index greater than or equal to its vertex count
- **THEN** the document is refused as malformed rather than loaded with a buffer a host would read past

#### Scenario: An index count that is not a multiple of three is refused
- **WHEN** a mesh chunk declares an index count that does not describe whole triangles
- **THEN** it is refused as malformed

#### Scenario: A well-formed mesh still loads
- **WHEN** a document written by this library containing mesh layers is loaded
- **THEN** it loads unchanged

### Requirement: A mesh chunk and its layer stay matched
A document SHALL write a mesh chunk only for a layer id that exists as a mesh-kind layer, and SHALL drop on load any mesh chunk whose layer id names no mesh layer. Geometry SHALL NOT be discarded when a layer is removed, because the inverse of a layer removal restores the layer by value and cannot carry the payload; the save and load filtering is what keeps an orphaned entry harmless.

#### Scenario: An orphaned payload is not written
- **WHEN** a mesh layer is removed and the document is saved
- **THEN** no mesh chunk is written for it, and the file carries no geometry for a layer it does not contain

#### Scenario: An unmatched chunk is dropped
- **WHEN** a document carrying a mesh chunk whose layer id names no mesh layer is loaded
- **THEN** the chunk is discarded and the document loads

#### Scenario: Removal is still undoable within a session
- **WHEN** a mesh layer is removed and the removal is undone before saving
- **THEN** the layer returns carrying the same geometry

### Requirement: A reader that predates mesh layers skips them
The container's major version SHALL NOT change: a mesh chunk is a new chunk type and unknown chunks are already skipped. The container minor and the scene minor SHALL both advance, because the layer record's kind byte gains a value, and the two are bound by a static assertion so they move together.

A reader written before mesh layers existed SHALL open such a document, skip the mesh chunks, and ignore any layer whose kind it does not recognise, exactly as it already ignores a layer that is not SDF. That reader SHALL lose the mesh layers if it saves the document again, and the format notes SHALL say so, as they already do for the losses earlier minors carry.

#### Scenario: An older reader opens a newer document
- **WHEN** a document containing mesh layers is opened by a reader written against the previous minor
- **THEN** it opens, the SDF and voxel layers are unchanged, and no mesh chunk is misread as something else

#### Scenario: A newer reader opens an older document
- **WHEN** a document written before mesh layers existed is loaded
- **THEN** it loads with no mesh layers and is otherwise exactly what it was

#### Scenario: No forward refusal
- **WHEN** a document containing mesh layers is opened
- **THEN** the major version is unchanged, so nothing is refused on version grounds

### Requirement: The per-axis scale is a gated appended field
The node record SHALL carry an item's per-axis scale from scene minor 14, appended after the fields the record already held rather than placed beside the transform, so that a build predating the field reads exactly the bytes it always did.

Writing a document AT minor 13 or below SHALL omit it, and the item SHALL then degrade to its UNIFORM scale — a squashed cylinder comes back round rather than missing. That is the recoverable direction and the one an older build can evaluate, and it SHALL be documented at the constant rather than left to be discovered.

Reading a document written at minor 13 or below SHALL leave the per-axis scale at its default of `(1, 1, 1)`, which is exactly what those documents already meant, so an older file's field is unchanged rather than reinterpreted.

The transform COMMAND SHALL carry the same three floats under the same gate, because that command carries the whole transform and a journal written at an older minor must replay on a build that predates the field as the uniform transform it always was.

#### Scenario: A squash round trips
- **WHEN** a document containing an item with a per-axis scale is serialized and read back
- **THEN** the scale is exactly what was written, and reserializing produces identical bytes

#### Scenario: An older minor drops it and keeps everything else
- **WHEN** the same document is serialized at minor 13
- **THEN** the bytes are identical to those of the same document with no per-axis scale, and reading them back gives an item with the default per-axis scale and its uniform scale intact

### Requirement: A sculpt layer stack is serialized with its surface
A sculpt layer stack SHALL serialize inside the multiresolution surface's versioned format — per layer: identity, name, kind, visibility, lock, strength, per-level detail blocks and any mask — and SHALL NOT be written into the flat mesh stream, whose readers expect interchange arrays.

Layer identities SHALL survive a save and a load, because a host, a journal and an undo step all hold them.

The layer kind SHALL be versioned from the first release, so that a later procedural layer does not require a format break.

The format SHALL remain BACKWARD-OPEN: a reader predating the stack SHALL open the document with the surface it can read rather than failing, and SHALL NOT silently present a partially composited surface as the whole one.

#### Scenario: A stack round-trips
- **WHEN** a surface carrying several layers with different strengths and visibilities is saved and reloaded
- **THEN** every layer's identity, properties and detail are restored, and the evaluated surface is identical

#### Scenario: An older reader does not misrepresent the surface
- **WHEN** a reader predating the stack opens a document containing one
- **THEN** it either reports that it cannot present the surface or presents it flattened, and does not present a partial composite as complete

### Requirement: A layer stack chunk is refused on its own terms, before it is reserved from
The stack chunk SHALL apply the same ceilings the surface stream around it applies to the same two numbers — a level count and a per-level vertex count. Both are numbers the layer decoder RESERVES FROM, and the surface's cross-check of a decoded stack against the hierarchy it rebuilt runs only after the layer decoder has returned. Where no such cross-check exists at all — a journal's structural undo record carries a whole stack snapshot and is not required to name the surface it was taken against — the decoder's own ceilings are the only refusal there is.

A stack's per-level invalidation index SHALL NOT be reserved from a declared level size. It is a cache read only while the level is partially stale, and a freshly decoded stack is wholly stale, so it SHALL be sized where it is first consulted.

A layer's per-level fields SHALL describe the stack's levels and SHALL share the stack's blocking. Block `b` naming the same vertices in a layer's coefficients, in its mask and in the level's composed field is what makes a strength change cost the layer's coverage rather than the surface, and the invalidation path hands a field's block numbers to the stack's index without translating them — so a stream pairing two blockings SHALL be refused rather than silently unsharing that index.

An unknown layer kind SHALL be refused rather than skipped. A strength outside `[0,1]`, including one that is not a number, SHALL be refused. Two layers answering to one identity, an identity at or above the serialized counter that mints the next one, and an active identity the stream does not carry SHALL each be refused.

#### Scenario: A hostile chunk costs its bytes rather than what it declares
- **WHEN** a stack chunk of under a hundred bytes declares the deepest hierarchy this build accepts, at the finest blocking the format allows, carrying no layers
- **THEN** it decodes, and the memory it reserves is proportional to the chunk rather than to the levels it names

#### Scenario: A journal snapshot naming an impossible hierarchy changes nothing
- **WHEN** a structural undo record whose stack snapshot declares a level larger than any level can be, or more levels than this build reconstructs, is replayed
- **THEN** the replay refuses, and the stack it was applied to is unchanged

#### Scenario: A stream pairing two blockings is refused
- **WHEN** a stack declaring one block size carries a layer whose coefficients or mask declare another
- **THEN** the stream is refused, rather than loading a stack whose invalidation would mark blocks the level does not have

### Requirement: The layer scale and the cage map are versioned

The scene and container minors SHALL move together for the layer's per-axis
scale and the lattice cage's affine map, and both fields SHALL be gated on the
writer's minor exactly as the radial fields and the shared-content id are: a
stream written at an older minor SHALL NOT carry fields that minor's reader will
not consume.

A stream written before this minor SHALL load with the layer scale at
(1, 1, 1) and the cage map at the rigid placement it recorded — both of which
are what those files always meant — rather than failing.

#### Scenario: An older document loads unchanged
- **WHEN** a document written at the previous minor is read
- **THEN** every layer carries a per-axis scale of ones, every transformed lattice carries the map its rigid placement described, and the document evaluates to what it evaluated to before

#### Scenario: A squashed layer round-trips
- **WHEN** a document holding a per-axis-scaled layer is written and read back
- **THEN** the three factors return exactly and the reloaded document evaluates bit-identically

#### Scenario: An older writer does not emit the new fields
- **WHEN** a document is written at a minor below this one
- **THEN** neither field is emitted and a reader at that minor consumes the stream without desynchronising

### Requirement: A payload several items share is stored once

Where several items hold ONE sampled payload — a sampled volume, or the mask
that gates an item — the document SHALL store it once and every holder SHALL
name it. Storing a copy per item makes a document that instances one asset grow
with the number of instances rather than with what it contains, which is the
difference between a reusable asset and one that can be used a handful of times.

**A reload SHALL preserve the sharing.** Deduplicating on write and rebuilding a
separate payload per item on read saves space in the file and restores the
duplication in memory, and the next save writes the copies again — so the two
halves are one requirement and not two.

The name a holder uses SHALL be unambiguous across the whole document. An
identifier that is only unique within a layer names a different item in another
layer, and the document model numbers each layer's items from one.

Sharing SHALL be identity, not equality: two payloads with equal contents that
were built separately are two payloads, exactly as two identical edit lists are.
Deduplicating by content would make the file's structure depend on a comparison
of megabytes and would silently merge two assets an artist may edit apart.

A document written for an older reader SHALL fall back to storing a payload per
item, so that reader opens it and gets what it always got. What such a downgrade
costs SHALL be size alone, never content.

#### Scenario: One asset placed many times
- **WHEN** a document places one captured payload many times and is saved
- **THEN** its size grows by a small record per placement rather than by the payload, and the payload appears once

#### Scenario: The sharing survives a reload
- **WHEN** such a document is loaded and saved again
- **THEN** every placement still refers to one payload, and the second file is the same size as the first

#### Scenario: An older reader still opens it
- **WHEN** the document is written for a reader that predates shared payloads
- **THEN** each item carries its own copy, the reader opens it, and the field every item contributes is unchanged

### Requirement: Every step in the history can be serialized
Each kind of step the session history records SHALL have a byte encoding, so that a journal can carry the whole session rather than the part of it the command vocabulary happens to cover.

The edit-list steps already have one — the command encoding the document format's scene chunk uses. The voxel steps are runs of cell writes and SHALL be encoded as such. The mesh steps are sparse vertex deltas and currently have **no** encoding; one SHALL be added, covering the vertices touched and their before and after positions, and their normals and colours where the record carries them.

A journal that could encode two of the three kinds would recover two thirds of a session and say nothing about the third, which is the failure this requirement exists to prevent.

#### Scenario: Every step kind round-trips
- **WHEN** a session containing edit-list, voxel and mesh steps is journaled and replayed
- **THEN** each kind is reconstructed, and the mesh layer's vertices, normals and colours match what they were

#### Scenario: A mesh step's record survives the round trip
- **WHEN** sparse vertex deltas are encoded and decoded
- **THEN** the decoded record reverts and re-applies a mesh exactly as the original did, and `indices` and `quads` are untouched by both

### Requirement: A serialized document knows which bytes it is
Serializing a document and loading one SHALL both record the identity of the bytes involved on the document, so a journal taken afterwards can name the snapshot it continues from and a replay can refuse a mismatched pair.

The identity SHALL be derived from the serialized bytes rather than minted per session: two snapshots with the same bytes are the same snapshot, a journal taken against one replays onto the other exactly, and a per-session token would refuse a pair that recovers perfectly.

It is NOT a checksum. It is not written into the file, it does not detect corruption, and it is not stable across builds that change the encoding — it answers one question about two things already in memory, and it SHALL cost a small fraction of the save that produces it rather than a comparable one.

#### Scenario: A snapshot and its reload agree
- **WHEN** a document is serialized and the same bytes are loaded back
- **THEN** both documents carry the same identity, and a journal taken from the first replays onto the second

#### Scenario: Two different documents do not
- **WHEN** two documents with different content are serialized
- **THEN** their identities differ, and a journal taken against one is refused against the other

