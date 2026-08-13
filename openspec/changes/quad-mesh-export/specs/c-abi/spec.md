# c-abi — a host asks for quads and learns what it got

Delta for `quad-mesh-export`.

## ADDED Requirements

### Requirement: Quad meshing across the ABI
The C ABI SHALL expose quad meshing for both sources: the document's SDF content and a voxel grid. Both SHALL take one versioned descriptor carrying the lattice cell size, an optional target quad count with its tolerance and iteration cap, and the mode.

The descriptor SHALL carry the leading `uint32_t struct_size` every descriptor in this ABI carries, so the count controls can be appended to later without a major bump.

The mode SHALL be checked against the declared list and an unknown value SHALL be rejected rather than mapped onto the default, as the mesher enum already is. The dual mode SHALL be zero, so a caller whose declared size predates the field gets the lattice dual.

The faces mode SHALL be voxels only. A document asked for it SHALL be refused with `CLAY_ERROR_INVALID_ARGUMENT` rather than quietly given the dual: a silent substitution of a smooth mesh for a boxy one is visible in the render and invisible in the return code.

These SHALL be NEW entry points. `clay_document_mesh`, `clay_document_mesh_combined`, `clay_voxel_mesh`, `clay_voxel_mesh_smooth` and `clay_voxel_mesh_chunks` SHALL return exactly what they return today, carrying no quads.

The C header SHALL state, at these entry points, that the output is a lattice-derived quad grid and NOT field-aligned retopology — no edge loops, no feature-placed poles, not animation-ready.

#### Scenario: A document quad-meshes
- **WHEN** a host calls the document quad mesher with a cell size and the dual mode
- **THEN** it receives a mesh whose quad count is non-zero and whose triangle indices are that quad list's triangulation

#### Scenario: Faces mode on a document is refused
- **WHEN** a host asks a document for the faces mode
- **THEN** the call returns `CLAY_ERROR_INVALID_ARGUMENT` with a detail message, and no mesh is produced

#### Scenario: A descriptor that predates the fields still meshes
- **WHEN** a caller declares the descriptor's original size
- **THEN** the call meshes with the dual mode and no target, the appended fields taking their zero defaults

#### Scenario: The existing meshers are untouched
- **WHEN** the existing document, voxel, smooth and per-chunk mesh calls run after quad meshing exists
- **THEN** each returns the same vertices and indices it returned before, and reports no quads

### Requirement: A mesh reports its quads and how it reached them
`clay_mesh` SHALL report its quad count, SHALL expose a borrowed pointer to the quad indices with the lifetime rule every other borrowed mesh pointer has, and SHALL offer a copy-into-a-caller-buffer form alongside the existing index copy, taking the exact element count for the same reason that one does.

A mesh carrying no quads SHALL report a count of zero and a null pointer, never a fabricated pairing of its triangles.

The existing accessors — vertex count, index count, positions, normals, colours, uvs, indices, the interleaved vertex copy, bounds, validation and save — SHALL be unaffected. The index accessors SHALL keep reporting the triangulation, because that is what a GPU consumer draws.

A mesh SHALL additionally report how it was produced: the lattice cell size it was meshed at, the target it was given (zero when none), the count it reached, the iterations the search spent, whether it landed inside the tolerance, and whether it clamped. This is the ONLY way a host learns that a target of fifty thousand produced thirty-one thousand because a ceiling stopped the search. Asking a mesh that was not quad-meshed SHALL be refused rather than answered with zeroes.

#### Scenario: A host reads the quads
- **WHEN** a host quad-meshes and reads the quad count and pointer
- **THEN** the pointer addresses four indices per quad, all within the vertex count, valid until the mesh is destroyed

#### Scenario: A triangle mesh reports no quads
- **WHEN** a host reads the quad count of a mesh produced by any existing mesher, loaded from a file, or built from triangles
- **THEN** the count is zero and the pointer is null

#### Scenario: The report explains the number the host got
- **WHEN** a host asks for a target the resolution ceiling cannot reach and then reads the report
- **THEN** the report states the count actually produced, the cell size used, and that the search clamped without reaching the tolerance

#### Scenario: A mesh that was never quad-meshed has no report
- **WHEN** a host asks a mesh loaded from a file for its quad report
- **THEN** the call is refused with an error code rather than answering with zeroes

### Requirement: Quads follow a mesh through the calls that copy one
A mesh transform SHALL keep the quads it was given: it moves positions and rotates normals and does not touch indices.

Concatenation SHALL carry quads only when EVERY input carries them, rebasing them onto the concatenated vertices as it rebases the triangles, and SHALL drop them entirely otherwise. This is the attribute-drop rule the header already states for normals, colours and uvs, applied for the same reason: a result that was quads over part of itself and triangles over the rest is not a quad mesh, and no call in this ABI may return a mesh whose arrays contradict each other.

Attaching a mesh as a document layer SHALL copy its quads with its geometry, and the borrowed mesh SHALL report them.

Saving SHALL write quads in the formats that carry them. The header SHALL state at the save entry point that OBJ, PLY and FBX carry quads and that GLB does not, because glTF 2.0 has no quad primitive mode.

#### Scenario: A transformed quad mesh is still a quad mesh
- **WHEN** a quad mesh is transformed
- **THEN** the result carries the same quad list over the moved positions

#### Scenario: Mixed concatenation drops quads
- **WHEN** a quad mesh is concatenated with a mesh carrying none
- **THEN** the result carries no quads and its triangles are the concatenation, exactly as before

#### Scenario: A quad mesh layer keeps its quads
- **WHEN** a quad mesh is added as a document layer and the layer's mesh is borrowed back
- **THEN** the borrowed mesh reports the same quad count and the same quad indices
