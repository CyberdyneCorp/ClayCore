# scene-model — Read Curve Points

Delta for `read-curve-points`.

## MODIFIED Requirements

### Requirement: Editing a curve is an ordinary edit
Replacing an item's point list SHALL be expressed as a command, so that it is undoable, serializable and refused on a protected layer like every other edit. Its inverse SHALL restore the previous list exactly. The command SHALL apply to a swept item's guide as well as to a stroke, since a guide is the same control-point list and not a new kind of curve; a node that carries no such list SHALL still be refused.

#### Scenario: Editing a curve is undoable
- **WHEN** a curve's points are replaced and the edit is undone
- **THEN** the document is exactly what it was

#### Scenario: A protected layer refuses a curve edit
- **WHEN** a curve on a locked layer has its points replaced
- **THEN** the edit is refused and the curve is unchanged

#### Scenario: A placed sweep's guide is editable
- **WHEN** a swept item's points are replaced with a differently shaped guide
- **THEN** the edit applies, and its inverse restores the guide that was there

#### Scenario: A node with no point list is refused
- **WHEN** the replace names a primitive that carries no control points
- **THEN** it fails and the document is untouched
