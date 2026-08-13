# file-io — a sign per armature node

Delta for `add-armature-node-signs` (#99).

## MODIFIED Requirements

### Requirement: The node record carries a tree
The node record SHALL carry an armature's parent indices alongside its points, gated on the minor so that a reader predating armatures is unaffected, and its signs the same way at the following minor, so that a reader predating signs is unaffected by an all-positive document written at its own minor.

Writing at a minor below the signs minor SHALL drop the signs and reproduce the older bytes exactly — the existing escape hatch for an older build — and a document whose armature is all-positive SHALL lose nothing to it.

#### Scenario: An older reader is not broken by an armature
- **WHEN** a reader that predates armatures opens a document containing one
- **THEN** it opens the document rather than refusing it, and the armature is absent rather than corrupt

#### Scenario: Signs round trip at the current minor
- **WHEN** a document holding an armature with a negative node is saved and reloaded at the current minor
- **THEN** the signs read back exactly and the document reserialises to identical bytes

#### Scenario: Writing at the previous minor drops only the signs
- **WHEN** the same document is written at the minor below the signs minor
- **THEN** the bytes are exactly what that minor produced before signs existed, and reloading them yields the all-positive rig
