## ADDED Requirements

### Requirement: A host can build a mesh carrying its own vertex attributes
The C ABI SHALL expose `clay_mesh_from_arrays`, taking a `clay_mesh_arrays` descriptor with a leading `struct_size`, positions, optional vertex-aligned normals, colours and uvs, and exactly one of a triangle index list or a quad index list. Every array SHALL be copied. A NULL attribute SHALL leave that attribute absent, and a supplied one SHALL read back bit-exactly through `clay_mesh_normals`, `clay_mesh_colors` and `clay_mesh_uvs`. A quad list SHALL derive the triangles by the `(a,b,c),(a,c,d)` rule `clay_mesh_from_quads` uses.

The call SHALL return `CLAY_ERROR_INVALID_ARGUMENT` and leave the out mesh NULL for a `struct_size` below the layout, NULL positions, zero vertices, neither or both index kinds, an index count that is not whole triangles or quads, or an index past the vertices. It is not required to validate attribute lengths, which a pointer cannot carry, and the header SHALL state this.

#### Scenario: Attributes read back exactly
- **WHEN** a host builds a mesh with uvs, normals and colours
- **THEN** each reader returns the same floats, bit for bit

#### Scenario: Attributes survive a layer and a document
- **WHEN** that mesh is attached as a mesh layer, the creation is undone and redone, and the document is saved and loaded
- **THEN** the layer's mesh returns the same floats at every step

#### Scenario: Absent attributes stay absent
- **WHEN** the attribute pointers are NULL
- **THEN** the readers return NULL

#### Scenario: Malformed calls are refused
- **WHEN** positions are NULL, an index count is not whole primitives, an index is out of range, both index kinds are supplied, or `struct_size` is undersized
- **THEN** the call returns `CLAY_ERROR_INVALID_ARGUMENT` and the out mesh is NULL
