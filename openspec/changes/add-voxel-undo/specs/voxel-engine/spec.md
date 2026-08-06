# voxel-engine delta

## ADDED Requirements

### Requirement: Voxel brush edits on document layers are undoable
The system SHALL record an inverse diff (affected cells with prior
palette slots) into the document's undo history for every brush edit
(clay_voxel_set_brush, clay_voxel_erase_brush, clay_voxel_paint_brush)
made on a grid borrowed from a document layer while undo is enabled,
interleaved in order with scene-command steps. Standalone grids SHALL
remain direct and unrecorded.

#### Scenario: Undo removes a brush stamp
- GIVEN a document with undo enabled and a voxel layer
- AND a `clay_voxel_set_brush` stamp on its borrowed grid
- WHEN `clay_document_undo` is called
- THEN the stamped cells SHALL return to their prior palette slots
- AND `clay_document_redo` SHALL restore the stamp exactly

#### Scenario: Undo ordering interleaves voxel and scene edits
- GIVEN an SDF stroke committed, then a voxel stamp
- WHEN `clay_document_undo` is called once
- THEN the voxel stamp SHALL be undone and the stroke SHALL remain
- AND a second undo SHALL remove the stroke

#### Scenario: Grouped brush calls undo as one step
- GIVEN `clay_document_begin_undo_group`, several brush calls, and
  `clay_document_end_undo_group`
- WHEN `clay_document_undo` is called once
- THEN every cell touched inside the group SHALL be restored

#### Scenario: Standalone grids stay outside history
- GIVEN a grid from `clay_voxel_grid_create`
- WHEN it is brushed and `clay_document_undo` is called on any document
- THEN the standalone grid SHALL be unchanged by the undo

### Requirement: Undo depth counts voxel steps
`clay_document_undo_state` SHALL report depths that include voxel steps,
so a host can label its buttons without tracking modes.

#### Scenario: Depth reflects a voxel stamp
- GIVEN an empty history and one voxel brush stamp
- WHEN `clay_document_undo_state` is queried
- THEN the undo depth SHALL be 1
