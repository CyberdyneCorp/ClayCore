# c-abi

## MODIFIED Requirements

### Requirement: Undo across the ABI
The C API SHALL expose the same opt-in history as the Python bindings: enable, undo, redo, depths and grouping. Calling undo with an empty history SHALL report that rather than failing, so a UI can drive it without tracking state itself.

Undo and redo SHALL act on the session history, which spans the SDF edit list, voxel grids, masks and mesh layers. On a mesh layer the ABI records what replaces its triangles — an attach, a replacement, a remesh; a vertex displacement made by a `clay_mesh_sculptor` is recorded into the host's `clay_mesh_deltas`, not into the document's history. Before this change they acted on the command stack alone, so a host calling them after a voxel or mesh edit reversed an older SDF edit instead of the edit the user had just made, or did nothing at all. That is a behaviour change and a fix: a host that already calls these gets the edits it was silently missing, through the same entry points.

The reported depths SHALL count steps that will actually reverse something, across every representation, so that a host greying out a menu item from a depth never offers an undo that does nothing.

An operation that no history mechanism records SHALL NOT be counted as a step, and a host SHALL be able to discover that such an operation lies in the history rather than inferring it from a step that does not do what was expected.

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

#### Scenario: Undo from Swift
- **WHEN** a C consumer enables undo, edits, and undoes
- **THEN** the document serializes bit-identically to its pre-edit state, matching what `pyclay` produces for the same sequence

#### Scenario: Empty stack is not an error
- **WHEN** undo is called on a document with nothing to undo
- **THEN** the call reports that nothing was undone without returning a failure code

#### Scenario: Undo reverses a voxel edit
- **WHEN** a host sculpts a voxel layer of a document and calls undo
- **THEN** the cells the pass changed are restored, where before this change the call reversed an unrelated SDF edit or reported nothing to undo

#### Scenario: Undo reverses a mesh layer's replacement
- **WHEN** a host replaces a mesh layer's triangles and calls undo
- **THEN** the previous triangles are restored and the layer's geometry revision advances

#### Scenario: A mesh sculptor stamp is not a document step
- **WHEN** a host stamps a `clay_mesh_sculptor` built over a document's mesh layer
- **THEN** the document's undo depth does not change, and the stamp is reversed through the `clay_mesh_deltas` the host passed to it rather than through `clay_document_undo`

#### Scenario: Depth counts every representation
- **WHEN** edits are made to the edit list, a voxel grid and a mesh layer's triangles
- **THEN** the reported undo depth counts them all, and calling undo that many times returns the document to its starting state

#### Scenario: The existing entry points keep their shape
- **WHEN** a host compiled against the previous ABI calls undo and redo
- **THEN** the calls have the same signatures and the same result codes, and reverse more than they used to rather than differently

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
The API SHALL expose voxel grids through an opaque handle: palette management, single-cell and batch edits, cube and sphere brushes with falloff and strength, the sculpting verbs (smooth, inflate, flatten, pinch), box and line fills, mirrored edits, flood select, occupancy and bounds queries, greedy meshing, SDF rasterization, and step-field sampling.

Ownership SHALL be explicit: a grid created standalone is owned by the caller and destroyed with an explicit destroy call, while a grid obtained as a document layer is borrowed, remains owned by the document, and SHALL NOT be destroyed by the caller. Destroying a borrowed handle SHALL return an error rather than corrupting the document.

A handle borrowed from a layer whose creation has been undone SHALL NOT be used:
the layer is absent from the document and the ABI's own lookup reports it as not
found. The cells are retained so that a redo restores them, not for a caller to
reach while the layer is gone.

#### Scenario: Voxel sculpting from C
- **WHEN** a C consumer creates a grid, adds palette entries, stamps a sphere brush, runs a sculpting verb, and greedy-meshes the result
- **THEN** the mesh matches the same sequence performed through `pyclay`

#### Scenario: Borrowed layer handle is protected
- **WHEN** a consumer calls destroy on a handle obtained from a document voxel layer
- **THEN** the call returns an invalid-argument error and the document is unaffected

#### Scenario: Batch edits use the size-query pattern
- **WHEN** flood select is called with a null buffer
- **THEN** it reports the required cell count, and a second call with an adequate buffer fills it

#### Scenario: Falloff brushes are reproducible across the boundary
- **WHEN** a C consumer stamps a falloff brush with a given seed
- **THEN** the affected cells are identical to the same stamp through `pyclay`

#### Scenario: Brush strength is passed through, never reinterpreted
- **WHEN** a C consumer stamps a brush at any strength the boundary accepts
- **THEN** the coverage reaches the engine untouched, so the affected cells are identical to the same stamp through `pyclay`
- **AND WHEN** the strength is not greater than zero, which covers no cell at all
- **THEN** the call returns an invalid-argument error and the grid is unchanged, rather than the value being read as full coverage

#### Scenario: A region with a non-finite bound is refused
- **WHEN** rasterization is asked for a region whose bounds contain a NaN or an infinity
- **THEN** the call returns an invalid-argument error and the grid is unchanged

#### Scenario: A borrowed grid outlives an undone creation
- **GIVEN** a voxel layer whose creation has been undone
- **WHEN** the document is asked for that layer by name
- **THEN** the lookup reports it as not found
