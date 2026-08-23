# scene-model

## ADDED Requirements

### Requirement: One undo order spans every representation
The document SHALL keep a single session history whose steps are ordered across the SDF edit list, voxel grids and mesh layers, so that one undo reverses the most recent edit whatever representation produced it.

Undoing SHALL take steps off in the reverse of the order they were made, and redoing SHALL put them back in that order, without regard to which representation each step belongs to. An SDF stamp, then a voxel smooth, then a mesh grab SHALL undo as mesh, voxel, SDF.

The history SHALL be an INDEX over the three existing mechanisms and SHALL NOT merge their storage. A voxel pass SHALL still be recorded as the cells it changed, a mesh displacement SHALL still be recorded as sparse vertex deltas, and an edit-list change SHALL still be recorded as a command inverse. Each of those was chosen for a reason that has not changed — a voxel edit has no compact inverse, and a vertex displacement is not an edit item — and a history that forced them into one representation would be re-deciding all three.

Each mechanism SHALL keep the behaviour it already has. Consecutive stroke commands on one node SHALL still coalesce into one step, an explicit group SHALL still bundle arbitrary commands into one step, and a voxel sculpt layer SHALL remain independently addressable.

#### Scenario: One undo crosses representations
- **WHEN** an SDF item is added, then a voxel region is smoothed, then a mesh layer's vertices are moved
- **THEN** the first undo restores the mesh vertices, the second restores the voxel cells, and the third removes the SDF item

#### Scenario: Redo restores the same order
- **WHEN** three such steps are undone and then redone
- **THEN** the document, the grid and the mesh each return to the state they had before the undos, and the step order is preserved

#### Scenario: Coalescing survives
- **WHEN** a stroke of many stamps is applied to an SDF layer and then undone once
- **THEN** the whole stroke is reversed as one step, exactly as it was before this change

#### Scenario: A voxel pass stays addressable
- **WHEN** a voxel pass is recorded and its strength is later changed
- **THEN** the pass remains addressable as a sculpt layer, and the strength change is its own history step rather than being confused with the pass that created it

### Requirement: The history says what it cannot reverse
An operation that no history mechanism records SHALL NOT appear as an undoable step, and the history SHALL be able to report that such an operation happened.

Consolidating a layer, rasterizing a document into a grid and converting between representations are destructive and are recorded by none of the three mechanisms. A history that silently omitted them would leave a host offering an undo that jumps over the operation the user most wants back; one that offered a step which did nothing would be worse. The boundary SHALL be stated in the specification and SHALL be observable at runtime rather than discovered.

#### Scenario: An unrecorded operation is not a silent gap
- **WHEN** an operation outside every history mechanism is performed between two recorded steps
- **THEN** undo reverses the recorded step before it, and a host can tell that an unreversible operation lies between them

#### Scenario: The step count matches what can be undone
- **WHEN** a host reads the history depth
- **THEN** it equals the number of steps that will actually reverse something, so a menu built from it never offers an undo that does nothing
