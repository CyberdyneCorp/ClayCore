# file-io

## ADDED Requirements

### Requirement: Every step in the history can be serialized
Each kind of step the session history records SHALL have a byte encoding, so that a journal can carry the whole session rather than the part of it the command vocabulary happens to cover.

The edit-list steps already have one — the command encoding the document format's scene chunk uses. The voxel steps are runs of cell writes and SHALL be encoded as such. The mesh steps are sparse vertex deltas and currently have **no** encoding; one SHALL be added, covering the vertices touched and their before and after positions, and their normals and colours where the record carries them.

A journal that could encode two of the three kinds would recover two thirds of a session and say nothing about the third, which is the failure this requirement exists to prevent.

#### Scenario: Every step kind round-trips
- **WHEN** a session containing edit-list, voxel and mesh steps is journaled and replayed
- **THEN** each kind is reconstructed, and the mesh layer's vertices, normals and colours match what they were

#### Scenario: A mesh step's record survives the round trip
- **WHEN** sparse vertex deltas are encoded and decoded
- **THEN** the decoded record reverts and re-applies a mesh exactly as the original did, and `indices` and `quads` are untouched by both
