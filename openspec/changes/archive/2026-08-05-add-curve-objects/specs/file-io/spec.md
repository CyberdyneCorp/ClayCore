# file-io — a versioned scene chunk

Delta for `add-curve-objects`.

## ADDED Requirements

### Requirement: The scene chunk carries a version
The scene payload SHALL be decoded against the container's minor version rather than assuming the current layout, so that a field added to a node does not require a packing trick to stay backward compatible. A document written at an earlier minor SHALL load with the new fields at their defaults.

#### Scenario: An older document loads with hard corners
- **WHEN** a document written before point types existed is loaded
- **THEN** every stroke point is a hard corner, no list is closed, and the field is what it always was

#### Scenario: Curves round trip
- **WHEN** a document containing a closed Bezier curve is saved and reloaded
- **THEN** the control points, their types, their handles, the closed flag and the tolerance all come back, and the field is unchanged
