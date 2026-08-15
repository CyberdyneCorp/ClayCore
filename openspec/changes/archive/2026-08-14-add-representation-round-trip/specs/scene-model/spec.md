# scene-model — move a sculpt between the two representations

Delta for `add-representation-round-trip`.

## ADDED Requirements

### Requirement: Conversion is an ordinary edit
Converting between representations SHALL go through the command vocabulary, so it is undoable and refused on a protected layer like any other edit.

#### Scenario: A conversion undoes
- **WHEN** a layer is converted and the edit is undone
- **THEN** the document returns to exactly what it was, including the procedural items the conversion would otherwise have discarded
