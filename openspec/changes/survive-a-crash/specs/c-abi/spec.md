# c-abi

## ADDED Requirements

### Requirement: A session's steps can be journaled and replayed
The C API SHALL let a host take the recorded steps since a named point as bytes, and SHALL let it replay such bytes onto a document.

Taking the journal SHALL be incremental: a host names the step index it has already persisted and receives everything after it, so an autosave costs the edits since the last one rather than the whole document. The engine SHALL report the index the host has now reached, so the next call continues from there.

Replay SHALL apply steps in the order they were recorded, onto a document that SHALL be the snapshot the journal was taken against. Replay SHALL report how many steps it applied, and SHALL stop rather than continue when a step cannot be applied, because a partially replayed journal that keeps going produces a document that matches neither the snapshot nor the session.

Journaled bytes SHALL be versioned, and a journal a build does not understand SHALL be refused rather than partially interpreted. A recovery that silently drops what it could not read is the failure this change exists to prevent.

The bytes SHALL be returned through the same owner handle every other serialized payload uses, so a host learns one lifetime rule.

#### Scenario: A session is reconstructed from a snapshot and a journal
- **WHEN** a document is snapshotted, further edits are made across the SDF edit list and a voxel layer, the journal is taken, and both are replayed onto a fresh document
- **THEN** the reconstructed document evaluates identically to the original at every probe point and its voxel layer holds the same cells

#### Scenario: The journal is incremental
- **WHEN** a host takes the journal, makes more edits, and takes it again from the index it was given
- **THEN** the second call returns only the steps made since the first, and replaying both in order reconstructs the session

#### Scenario: Replay reports what it applied
- **WHEN** a journal is replayed onto a document
- **THEN** the number of steps applied is reported, and a step that cannot be applied stops the replay rather than being skipped

#### Scenario: An unreadable journal is refused
- **WHEN** a journal written by a newer build, or a truncated one, is replayed
- **THEN** it is refused with a typed error and the document is left as it was, rather than partially applied

### Requirement: A journal says when it stops being enough
A journal SHALL make an unreversible operation visible, and a host SHALL be able to tell that its journal can no longer reconstruct the session on its own.

The history records operations no mechanism can reverse. Replay cannot reconstruct past one, so the journal SHALL mark it and the host SHALL be told that appending is no longer sufficient and the document must be snapshotted again.

A host that ignores the mark and replays anyway SHALL get a refusal at that point rather than a document that is quietly missing the operation's effect.

#### Scenario: A barrier tells the host to re-snapshot
- **WHEN** an operation that no mechanism records happens during a session
- **THEN** the host can tell from the journal that a new snapshot is required, before it relies on the journal to recover

#### Scenario: Replaying past a barrier is refused, not approximated
- **WHEN** a journal containing a barrier is replayed
- **THEN** replay stops at the barrier and reports it, rather than continuing and producing a document missing that operation's effect
