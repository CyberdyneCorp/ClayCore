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

### Requirement: A serialized document knows which bytes it is
Serializing a document and loading one SHALL both record the identity of the bytes involved on the document, so a journal taken afterwards can name the snapshot it continues from and a replay can refuse a mismatched pair.

The identity SHALL be derived from the serialized bytes rather than minted per session: two snapshots with the same bytes are the same snapshot, a journal taken against one replays onto the other exactly, and a per-session token would refuse a pair that recovers perfectly.

It is NOT a checksum. It is not written into the file, it does not detect corruption, and it is not stable across builds that change the encoding — it answers one question about two things already in memory, and it SHALL cost a small fraction of the save that produces it rather than a comparable one.

#### Scenario: A snapshot and its reload agree
- **WHEN** a document is serialized and the same bytes are loaded back
- **THEN** both documents carry the same identity, and a journal taken from the first replays onto the second

#### Scenario: Two different documents do not
- **WHEN** two documents with different content are serialized
- **THEN** their identities differ, and a journal taken against one is refused against the other
