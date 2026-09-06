# c-abi

## ADDED Requirements

### Requirement: A session's steps can be journaled and replayed
The C API SHALL let a host take the recorded steps since a named point as bytes, and SHALL let it replay such bytes onto a document.

Taking the journal SHALL be incremental: a host names the step index it has already persisted and receives everything after it, so an autosave costs the edits since the last one rather than the whole document. The engine SHALL report the index the host has now reached, so the next call continues from there.

Replay SHALL apply steps in the order they were recorded, onto a document that SHALL be the snapshot the journal was taken against. Replay SHALL report how many steps it applied, and SHALL stop rather than continue when a step cannot be applied, because a partially replayed journal that keeps going produces a document that matches neither the snapshot nor the session.

Journaled bytes SHALL be versioned, and a journal a build does not understand SHALL be refused rather than partially interpreted. A recovery that silently drops what it could not read is the failure this change exists to prevent.

The bytes SHALL be returned through the same owner handle every other serialized payload uses, so a host learns one lifetime rule.

Journaled bytes SHALL carry the identity of the snapshot they continue from, and replay onto a document that is not that snapshot SHALL be refused with a result code of its own, distinct from the one an unreadable journal returns, and with nothing applied. The two mean opposite things to a host: unreadable says discard the file, mismatched says find the snapshot it belongs to. The identity SHALL be recorded by the calls that serialize and load a document, so a host receives the check without asking for it, and SHALL be the identity of the snapshot current at the index the journal was taken from rather than of the most recent one — a host that serializes again to compare the journal against the document must not have the journal it already asked for repointed at an image it discarded.

A journal that names no snapshot SHALL NOT be refused: journals written before the identity existed carry none, and refusing those would turn an upgrade into the data loss this change exists to prevent.

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

#### Scenario: A journal paired with the wrong snapshot is refused
- **WHEN** a journal is replayed onto a document loaded from a different snapshot, or onto one that was never serialized at all
- **THEN** it is refused with a result code distinct from an unreadable journal's, and the document is byte-identical afterwards

#### Scenario: The snapshot named is the one current at the index
- **WHEN** a host takes the journal from an index, and the document is serialized again afterwards
- **THEN** the journal already taken still names the snapshot that was current at that index, and is refused against the later image

### Requirement: A journal says when it stops being enough
A journal SHALL make an unreversible operation visible, and a host SHALL be able to tell that its journal can no longer reconstruct the session on its own.

The history records operations no mechanism can reverse. Replay cannot reconstruct past one, so the journal SHALL mark it and the host SHALL be told that appending is no longer sufficient and the document must be snapshotted again.

The C API SHALL let a host ASK whether the events since an index contain such an operation, and where. Reporting one only from replay is not enough: replay happens during the recovery, which is the one moment when being told to take a fresher snapshot is useless, because the session that would have been snapshotted is gone.

A host that ignores the mark and replays anyway SHALL get a refusal at that point rather than a document that is quietly missing the operation's effect.

#### Scenario: A barrier tells the host to re-snapshot
- **WHEN** an operation that no mechanism records happens during a session
- **THEN** the host can tell from the journal that a new snapshot is required, before it relies on the journal to recover

#### Scenario: Dropping a resolution level is one such operation
- **WHEN** a host drops a voxel resolution level on a document with undo enabled
- **THEN** the journal marks it, and a replay across it stops there rather than rebuilding a grid that still holds the level

#### Scenario: Replaying past a barrier is refused, not approximated
- **WHEN** a journal containing a barrier is replayed
- **THEN** replay stops at the barrier and reports it, rather than continuing and producing a document missing that operation's effect
