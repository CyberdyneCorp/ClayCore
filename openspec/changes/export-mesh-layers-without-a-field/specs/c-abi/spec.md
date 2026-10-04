## MODIFIED Requirements

### Requirement: Exporting a document with its imported meshes is explicit
Meshing a document SHALL continue to mean meshing its field, unchanged: it prices a dense grid from the tape's own bounds, and geometry that is not in the tape would either inflate that grid or fall outside it, and would change what an existing call returns for an existing document. Voxel layers are already outside it for the same reason.

The ABI SHALL instead expose transforming a mesh and concatenating meshes, plus one call that meshes the field and appends every visible mesh layer under its layer transform. Concatenation SHALL rebase indices. An attribute present on some inputs and absent on others SHALL be dropped from the result, because no mesh may be returned whose normals, colors or uvs are non-empty and a different length than its positions; the drop SHALL be documented at the call rather than discovered afterwards.

A mesh layer that is not visible SHALL be excluded from the combined export. Ghost and lock SHALL NOT change what is exported, consistent with neither flag changing what a document evaluates to.

When at least one mesh layer is visible, an empty field -- no visible SDF geometry, or a field whose every surface group is hidden -- SHALL contribute nothing to the combined export rather than refuse it; the result SHALL be the placed mesh layers alone. When no mesh layer is visible, the combined export SHALL return exactly what meshing the field returns, refusals included.

#### Scenario: Meshing a document is what it always was
- **WHEN** a document containing mesh layers is meshed with the existing call
- **THEN** the result is bit-identical to the same document without them

#### Scenario: A sculpt exports beside its reference model
- **WHEN** a consumer asks for the combined export of a document holding both an SDF layer and a mesh layer
- **THEN** the result contains both, with the imported triangles placed under their layer transform and their indices rebased

#### Scenario: A mismatched attribute is dropped, not truncated
- **WHEN** a mesh carrying uvs is concatenated with one that carries none
- **THEN** the result carries no uvs, rather than an array shorter than its positions

#### Scenario: A hidden mesh layer is not exported
- **WHEN** a mesh layer is hidden and the document is exported
- **THEN** its triangles are absent, while ghosting or locking it changes nothing

#### Scenario: A document whose only visible geometry is mesh layers exports those layers
- **WHEN** every SDF layer of a document is hidden, or it has none, and a consumer asks for the combined export while a mesh layer is visible
- **THEN** the call succeeds and the result is that mesh layer's triangles under its layer transform, where meshing the field alone is still refused

#### Scenario: A document with nothing visible is still refused
- **WHEN** a document has an empty field and no visible mesh layer, and a consumer asks for the combined export
- **THEN** the call refuses with the same error meshing the field returns, and writes no mesh
