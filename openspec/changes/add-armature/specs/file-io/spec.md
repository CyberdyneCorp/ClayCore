# file-io — armatures

Delta for `add-armature`.

## ADDED Requirements

### Requirement: The node record carries a tree
The node record SHALL carry an armature's parent indices alongside its points, gated on the minor so that a reader predating armatures is unaffected.

#### Scenario: An older reader is not broken by an armature
- **WHEN** a reader that predates armatures opens a document containing one
- **THEN** it opens the document rather than refusing it, and the armature is absent rather than corrupt
