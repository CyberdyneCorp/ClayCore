# file-io — the polygon formats write polygons

Delta for `quad-mesh-export`.

## ADDED Requirements

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
