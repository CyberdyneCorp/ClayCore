# c-abi

## MODIFIED Requirements

### Requirement: Undo across the ABI

The C API SHALL expose the same opt-in undo stack as the Python bindings:
enable, undo, redo, depths and grouping. Calling undo with an empty stack SHALL
report that rather than failing, so a UI can drive it without tracking state
itself.

EVERY layer creation the ABI exposes SHALL record its inverse.
`clay_add_sdf_layer` and `clay_document_add_mesh_layer` already applied
`AddLayerCmd`; `clay_document_add_voxel_layer` SHALL do the same rather than
mutating the document directly, so that a conversion into a new voxel layer is
reversible as a whole rather than in the half that happened to record.

Undoing a voxel or mesh layer's creation SHALL remove the layer and SHALL retain
its payload, so that a redo restores the layer with its content and the same id.
The payload SHALL NOT be reachable through the ABI while the layer is absent.

An explicit `clay_document_begin_undo_group` / `clay_document_end_undo_group`
bracket SHALL be ONE step across every representation it spans — edit list,
voxel grid, mask and mesh — and not one step per representation. A bracket that
produced a single step SHALL be unchanged by the grouping, and a bracket
containing an operation that nothing records SHALL leave that operation as its
own step, so that an undo is never offered across a barrier.

#### Scenario: A C consumer undoes an edit
- **WHEN** a C consumer enables undo, edits, and undoes
- **THEN** the edit is reversed and the depths report what remains

#### Scenario: Nothing to undo
- **WHEN** undo is called on a document with nothing to undo
- **THEN** the call reports that nothing was undone without returning a failure code

#### Scenario: Creating a voxel layer is an undo step
- **GIVEN** a document with undo enabled and nothing recorded
- **WHEN** a voxel layer is added
- **THEN** the undo depth is one
- **AND** undoing removes the layer from the document

#### Scenario: A crossing undoes as one step
- **GIVEN** a document with undo enabled holding a starting form
- **WHEN** a voxel layer is created and rasterized into inside one undo group
- **THEN** the undo depth grows by exactly one
- **AND** a single undo removes the layer and the cells together
- **AND** no empty layer is left in the document

#### Scenario: Redo restores the layer and its cells
- **GIVEN** a bracketed crossing that has been undone
- **WHEN** the document is redone once
- **THEN** the layer is present with the id it had
- **AND** it holds the cells the rasterization produced

#### Scenario: An ungrouped crossing stays two steps
- **GIVEN** a document with undo enabled
- **WHEN** a voxel layer is created and rasterized into without a bracket
- **THEN** the undo depth grows by two
- **AND** the first undo empties the layer and the second removes it

### Requirement: Voxel grids across the ABI

A voxel grid borrowed from a document SHALL remain owned by the document, and
SHALL NOT be destroyed by the caller.

A handle borrowed from a layer whose creation has been undone SHALL NOT be used:
the layer is absent from the document and the ABI's own lookup reports it as not
found. The cells are retained for a redo, not for a caller to reach.

#### Scenario: A borrowed grid outlives an undone creation
- **GIVEN** a voxel layer whose creation has been undone
- **WHEN** the document is asked for that layer by name
- **THEN** the lookup reports it as not found
