# python-bindings — editing an existing document

Delta for `add-edit-commands`.

## ADDED Requirements

### Requirement: Node and layer editing from Python
The module SHALL expose editing of an existing document, not only construction: for a node, setting its transform, primitive, colour, op/blend/rounding, moving it to a new parent and index, and removing it; for a layer, adding, removing, reordering, setting visibility and setting its transform; and for a stroke, appending points and trimming the last N. Every edit SHALL be addressed by node or layer id, and ids SHALL remain valid across unrelated edits.

#### Scenario: Moving a placed item
- **WHEN** a script adds a sphere, keeps its node id, and later sets a new transform on that id
- **THEN** the field reflects the new position and the node id is unchanged

#### Scenario: Removing an item
- **WHEN** a script removes a node by id
- **THEN** the node is gone from the layer and the remaining nodes keep their ids

#### Scenario: Layer visibility
- **WHEN** a layer is set invisible
- **THEN** the document evaluates as though that layer's content were absent, and setting it visible again restores the original field exactly

#### Scenario: Editing a stroke in place
- **WHEN** points are appended to an existing stroke and then the last N are trimmed
- **THEN** the stroke's field matches a stroke authored with the surviving points

#### Scenario: Every edit goes through the command vocabulary
- **WHEN** any editing entry point is called
- **THEN** it applies commands from `scene::Command` rather than mutating the document directly, so its semantics match what the document format records. Layer reorder SHALL be documented as the remove-then-add pair the vocabulary expresses it with; every other edit is a single command.
