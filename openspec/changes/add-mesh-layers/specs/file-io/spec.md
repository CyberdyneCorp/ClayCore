# file-io — the container carries imported meshes

Delta for `add-mesh-layers`.

## ADDED Requirements

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

## MODIFIED Requirements

### Requirement: Document format (.clayspace)
`clay::io` SHALL read and write the `.clayspace` single-file binary chunked container: versioned chunks for scene commands (the undo command vocabulary), palettes, voxel grids (palette+RLE compressed), imported meshes, thumbnails (PNG), and camera bookmarks. Readers SHALL open any older format version (backward-open) and SHALL refuse newer major versions with a clear error (forward-refuse), never crashing or partially loading. The format lives entirely in claycore so Python and CI read/write projects without the app.

#### Scenario: Round trip
- **WHEN** any golden-corpus document is saved and reloaded
- **THEN** the reloaded document evaluates bit-identically (same tapes, same brick results) and serializes to identical bytes

#### Scenario: Forward refusal
- **WHEN** a file with a higher major format version is opened
- **THEN** loading fails with a version-mismatch error code and no partial document is produced
