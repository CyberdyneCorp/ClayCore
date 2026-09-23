# scene-model

## MODIFIED Requirements

### Requirement: One undo order spans every representation
The document SHALL keep a single session history whose steps are ordered across the SDF edit list, voxel grids and mesh layers, so that one undo reverses the most recent edit whatever representation produced it.

Undoing SHALL take steps off in the reverse of the order they were made, and redoing SHALL put them back in that order, without regard to which representation each step belongs to. An SDF stamp, then a voxel smooth, then a mesh grab SHALL undo as mesh, voxel, SDF.

The history SHALL be an INDEX over the three existing mechanisms and SHALL NOT merge their storage. A voxel pass SHALL still be recorded as the cells it changed, a mesh displacement SHALL still be recorded as sparse vertex deltas, and an edit-list change SHALL still be recorded as a command inverse. Each of those was chosen for a reason that has not changed — a voxel edit has no compact inverse, and a vertex displacement is not an edit item — and a history that forced them into one representation would be re-deciding all three.

Each mechanism SHALL keep the behaviour it already has. Consecutive stroke commands on one node SHALL still coalesce into one step, an explicit group SHALL still bundle arbitrary commands into one step, and a voxel sculpt layer SHALL remain independently addressable.

An operation on a voxel sculpt-layer stack — a strength, a visibility, a reorder, a removal or a merge-down — SHALL be a step of its own kind, distinct from the pass it acts on. Its step SHALL carry the property it changed and every cell its recompose rewrote, and undoing or redoing it SHALL restore both without recomposing, so that the grid and its stack return to exactly the state they had. A merge-down SHALL hold the pass it folded, since undoing it means restoring that pass. An operation that changed nothing SHALL NOT be a step.

Enabling the history mid-session SHALL start an empty history and SHALL NOT be refused: what the document holds at that moment is the starting state, and enabling a history that is already enabled SHALL keep it. Closing a group that was never opened SHALL fold nothing, and only the outermost of nested groups SHALL fold.

An explicit group SHALL bundle arbitrary STEPS, of any representation, into one
step — not merely the scene commands inside it. The wrapped command stack
already collapses the commands of a bracket into one entry; a bracket SHALL do
the same for the kinds that stack cannot see, so that one gesture a host
bracketed is one undo however many representations it touched.

A step folded from a bracket SHALL apply its parts backwards on undo and
forwards on redo, and SHALL be all-or-nothing: if any part refuses, the parts
already applied SHALL be restored and the step SHALL remain on the stack.

An operation nothing records SHALL NOT be folded into a group. It stays its own
step, so that the horizon a host draws from it is not crossed by an undo.

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

#### Scenario: A voxel layer dial undoes to its setting
- **GIVEN** a voxel sculpt layer holding a pass, at full strength
- **WHEN** its strength is set to 0.4 and the history is undone once
- **THEN** the strength reads 1.0, the layer and its pass are still present, and the grid and stack are byte-identical to their state before the dial
- **AND** a redo returns them byte-identically to the dialled state

#### Scenario: Undoing a merge-down restores both layers
- **WHEN** a voxel sculpt layer is merged down into an overlapping one and the history is undone once
- **THEN** both layers are present with their names, strengths and recorded cells, and the grid and stack are byte-identical to their state before the merge

#### Scenario: Enabling mid-session starts empty
- **GIVEN** a document edited while its history was off
- **WHEN** the history is enabled
- **THEN** the undo depth is zero and the edits made before are the starting state
- **AND** enabling it again keeps any steps recorded since

#### Scenario: An unmatched group end folds nothing
- **GIVEN** three recorded steps and no open group
- **WHEN** a group is closed
- **THEN** the undo depth is still three

#### Scenario: A group holds a command and a voxel pass
- **GIVEN** a history with undo enabled
- **WHEN** a bracket contains one scene command and one voxel step
- **THEN** the undo depth is one
- **AND** one undo reverses both

#### Scenario: A group of commands alone is unchanged
- **GIVEN** a history with undo enabled
- **WHEN** a bracket contains only scene commands
- **THEN** it records exactly one step, as it did before groups spanned kinds

#### Scenario: A barrier in a group is not swallowed
- **GIVEN** a history with undo enabled
- **WHEN** a bracket contains a voxel step and an operation nothing records
- **THEN** the unrecordable operation is still its own step
- **AND** the undo depth stops at it

#### Scenario: A part that refuses leaves the step unapplied
- **GIVEN** a folded group whose voxel part names a layer that cannot be resolved
- **WHEN** the step is undone
- **THEN** the undo is refused
- **AND** the scene part it had already reversed is restored
