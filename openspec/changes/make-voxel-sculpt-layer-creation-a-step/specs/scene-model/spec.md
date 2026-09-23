## ADDED Requirements

### Requirement: A voxel sculpt layer's creation and the edits inside it are steps
Creating a voxel sculpt layer SHALL be a history step: undoing it SHALL remove the layer from the stack and end any recording into it, and redoing it SHALL restore the layer, with its name and dither seed, without reopening the recording.

An edit made while a voxel sculpt layer is recording SHALL be one step that carries both the cells it wrote and what it did to the layer's record of the pass. Undoing it SHALL restore the cells and the record together, so that the grid and its stack are byte-identical to their state before the edit and a later change to the layer's strength or visibility SHALL NOT re-apply the undone cells.

A journal replayed onto a snapshot taken before a voxel sculpt layer existed SHALL rebuild the layer and its passes, and SHALL NOT be refused at an operation that names it.

An edit inside a voxel sculpt layer that changed no cell SHALL NOT be a step, and SHALL leave the layer's record as it found it.

#### Scenario: Undoing an edit takes it out of the layer
- **GIVEN** a document with undo enabled and a voxel sculpt layer recording
- **WHEN** an inflate is made inside the layer and the history is undone once
- **THEN** the layer's cell count is zero and the document is byte-identical to its state before the inflate
- **AND** setting the layer's strength to 0.5 afterwards leaves the occupied cell count unchanged

#### Scenario: Undoing a creation removes the layer
- **GIVEN** a document with undo enabled
- **WHEN** a voxel sculpt layer is created, a pass is made inside it, and the history is undone twice
- **THEN** the grid has no sculpt layers and the document is byte-identical to its state before the creation
- **AND** redoing twice returns the layer and its pass byte-identically

#### Scenario: A journal rebuilds a layer the snapshot lacks
- **GIVEN** a snapshot taken before any voxel sculpt layer existed
- **WHEN** two layers are created with a pass each, one is dialled, and the journal since the snapshot is replayed onto it
- **THEN** every event applies and the recovered document is byte-identical to the live one

#### Scenario: A no-op edit inside a layer leaves no trace
- **GIVEN** a voxel sculpt layer recording, with undo enabled
- **WHEN** an edit that changes no cell is made inside it
- **THEN** the undo depth does not change and the grid and its stack are byte-identical to their state before the edit
