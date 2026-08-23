# c-abi

## MODIFIED Requirements

### Requirement: Undo across the ABI
The C API SHALL expose the same opt-in history as the Python bindings: enable, undo, redo, depths and grouping. Calling undo with an empty history SHALL report that rather than failing, so a UI can drive it without tracking state itself.

Undo and redo SHALL act on the session history, which spans the SDF edit list, voxel grids and mesh layers. Before this change they acted on the command stack alone, so a host calling them after a voxel or mesh edit reversed an older SDF edit instead of the edit the user had just made, or did nothing at all. That is a behaviour change and a fix: a host that already calls these gets the edits it was silently missing, through the same entry points.

The reported depths SHALL count steps that will actually reverse something, across every representation, so that a host greying out a menu item from a depth never offers an undo that does nothing.

An operation that no history mechanism records SHALL NOT be counted as a step, and a host SHALL be able to discover that such an operation lies in the history rather than inferring it from a step that does not do what was expected.

#### Scenario: Undo from Swift
- **WHEN** a C consumer enables undo, edits, and undoes
- **THEN** the document serializes bit-identically to its pre-edit state, matching what `pyclay` produces for the same sequence

#### Scenario: Empty stack is not an error
- **WHEN** undo is called on a document with nothing to undo
- **THEN** the call reports that nothing was undone without returning a failure code

#### Scenario: Undo reverses a voxel edit
- **WHEN** a host sculpts a voxel layer of a document and calls undo
- **THEN** the cells the pass changed are restored, where before this change the call reversed an unrelated SDF edit or reported nothing to undo

#### Scenario: Undo reverses a mesh edit
- **WHEN** a host moves a mesh layer's vertices and calls undo
- **THEN** the vertices are restored, and `indices` and `quads` are byte-identical throughout

#### Scenario: Depth counts every representation
- **WHEN** edits are made to all three representations
- **THEN** the reported undo depth counts them all, and calling undo that many times returns the document to its starting state

#### Scenario: The existing entry points keep their shape
- **WHEN** a host compiled against the previous ABI calls undo and redo
- **THEN** the calls have the same signatures and the same result codes, and reverse more than they used to rather than differently
