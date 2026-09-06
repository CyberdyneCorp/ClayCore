# c-abi — mesh layers reach the consumer

Delta for `add-mesh-layers`.

## ADDED Requirements

### Requirement: Mesh layers across the ABI
The API SHALL expose mesh layers: attaching an already-loaded mesh to a document as a layer, looking one up, and asking a mesh for its bounds. Attaching SHALL take a mesh handle rather than a path, because loading already exists and already owns import policy including the budget, and because taking a handle composes with building a mesh from triangles a host generated itself.

Attaching SHALL accept its own budget, tighter than the loader's defaults, since what a document may carry is a different question from what a file may decode into. It SHALL also accept an optional uniform import scale, applied to the vertices as they are stored, so unit conversion is resolved once at import rather than approximated by a layer transform.

Attaching SHALL go through the same layer vocabulary every other layer creation uses, so it is undoable and serializes with the document. Looking up a layer that does not exist, or that is not a mesh layer, SHALL return a not-found error.

Bounds SHALL be answered from the mesh handle, not from the layer bounds query. Layer bounds are derived from SDF shapes and the picking module may not see mesh data at all, so a bound computed there would either be empty or would require the module boundary to be widened for a query that has nowhere else to live.

#### Scenario: A host imports a model and keeps it
- **WHEN** a C consumer loads a mesh, attaches it to a document, saves and reloads
- **THEN** the mesh layer is present with the same geometry and the same transform

#### Scenario: Attaching is undoable
- **WHEN** a mesh is attached and the edit is undone
- **THEN** the layer is gone and the document matches what it was

#### Scenario: A budget refuses an oversized attach
- **WHEN** a mesh larger than the attach budget is attached
- **THEN** the call fails with a budget error and the document is unchanged

#### Scenario: A missing mesh layer is not found
- **WHEN** a consumer asks for a mesh layer by a name no mesh layer carries
- **THEN** the call returns a not-found error

#### Scenario: Framing an imported model
- **WHEN** a consumer asks a mesh handle for its bounds
- **THEN** it receives the box enclosing the mesh's positions, which layer bounds could not report

### Requirement: A mesh obtained from a document is borrowed
A mesh obtained as a document layer SHALL be borrowed: it remains owned by the document, stays valid until the document changes it or is destroyed, and SHALL NOT be freed by the caller. Destroying a borrowed mesh handle SHALL be a no-op that leaves the document intact.

It is a no-op rather than a reported refusal because the mesh destroy call returns no status, and changing its signature would break every existing consumer for a case that cannot arise today. Only handles that could not previously exist are affected. The header SHALL state this beside the existing lifetime note, as the borrowed voxel handle is documented beside its own.

#### Scenario: Destroying a borrowed mesh does nothing
- **WHEN** a consumer calls destroy on a mesh handle obtained from a document layer
- **THEN** the document is unaffected and its geometry is still readable through a fresh lookup

#### Scenario: An owned mesh is still freed
- **WHEN** a consumer destroys a mesh it loaded or built itself
- **THEN** it is freed exactly as before

### Requirement: Exporting a document with its imported meshes is explicit
Meshing a document SHALL continue to mean meshing its field, unchanged: it prices a dense grid from the tape's own bounds, and geometry that is not in the tape would either inflate that grid or fall outside it, and would change what an existing call returns for an existing document. Voxel layers are already outside it for the same reason.

The ABI SHALL instead expose transforming a mesh and concatenating meshes, plus one call that meshes the field and appends every visible mesh layer under its layer transform. Concatenation SHALL rebase indices. An attribute present on some inputs and absent on others SHALL be dropped from the result, because no mesh may be returned whose normals, colors or uvs are non-empty and a different length than its positions; the drop SHALL be documented at the call rather than discovered afterwards.

A mesh layer that is not visible SHALL be excluded from the combined export. Ghost and lock SHALL NOT change what is exported, consistent with neither flag changing what a document evaluates to.

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

## MODIFIED Requirements

### Requirement: The import budget is settable across the ABI
An importer's guardrail SHALL be settable by a C caller, not only enforced against the library's defaults, because the point of a budget is to be tightened for input the caller does not trust. It SHALL be checked against a file's DECLARED counts before anything is allocated.

A null budget SHALL mean the library's defaults, and a zeroed field SHALL mean the default for that field rather than "allow nothing", since a zeroed descriptor would otherwise refuse every file.

The budget SHALL also carry a file-byte ceiling, and loading a document SHALL be reachable with a budget. The document loader beneath the ABI has always taken one and the boundary offered no way to pass it, which was academic while a document held tapes and sparse grids; a document that embeds imported meshes makes the ceiling real, and a document's read ceiling is the caller's to raise. The budget-taking loader SHALL be a new entry point beside the existing one rather than a changed arity, so that no consumer has to be recompiled to keep working.

#### Scenario: A tight budget is enforced
- **WHEN** a mesh is loaded with a budget smaller than the file declares
- **THEN** the load fails with a budget error and nothing is allocated

#### Scenario: A null budget uses the defaults
- **WHEN** a mesh is loaded with no budget given
- **THEN** it loads under the library's defaults

#### Scenario: A zeroed field means the default
- **WHEN** a mesh is loaded with a budget whose fields are zero
- **THEN** it loads rather than being refused

#### Scenario: A document above the caller's ceiling is refused
- **WHEN** a document is loaded through the budget-taking entry point with a file ceiling below its size
- **THEN** it fails with a budget error

#### Scenario: The same document loads with a raised ceiling
- **WHEN** the same document is loaded with a ceiling above its size
- **THEN** it loads, so a document this library wrote is never permanently unopenable through the boundary

#### Scenario: The existing loader is unchanged
- **WHEN** a consumer compiled against the previous header loads a document
- **THEN** the call has the same signature and the same behaviour it always had
