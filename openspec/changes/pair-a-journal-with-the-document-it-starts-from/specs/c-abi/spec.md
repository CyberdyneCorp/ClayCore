## MODIFIED Requirements

### Requirement: A session's steps can be journaled and replayed
The C API SHALL let a host take the recorded steps since a named point as bytes, and SHALL let it replay such bytes onto a document.

Taking the journal SHALL be incremental: a host names the step index it has already persisted and receives everything after it, so an autosave costs the edits since the last one rather than the whole document. The engine SHALL report the index the host has now reached, so the next call continues from there.

Replay SHALL apply steps in the order they were recorded, onto a document that SHALL be the snapshot the journal was taken against. Replay SHALL report how many steps it applied, and SHALL stop rather than continue when a step cannot be applied, because a partially replayed journal that keeps going produces a document that matches neither the snapshot nor the session.

Journaled bytes SHALL be versioned, and a journal a build does not understand SHALL be refused rather than partially interpreted. A recovery that silently drops what it could not read is the failure this change exists to prevent.

The bytes SHALL be returned through the same owner handle every other serialized payload uses, so a host learns one lifetime rule.

Journaled bytes SHALL carry the identity of the snapshot they continue from, and replay onto a document that is not that snapshot SHALL be refused with a result code of its own, distinct from the one an unreadable journal returns, and with nothing applied. The two mean opposite things to a host: unreadable says discard the file, mismatched says find the snapshot it belongs to. The identity SHALL be recorded by the calls that serialize and load a document, so a host receives the check without asking for it, and SHALL be the identity of the snapshot current at the index the journal was taken from rather than of the most recent one — a host that serializes again to compare the journal against the document must not have the journal it already asked for repointed at an image it discarded.

Enabling undo SHALL seed the journal with the snapshot the document names only while the document is still that snapshot. A document edited after it was loaded or saved and before undo was enabled is not that snapshot, and its journal SHALL NOT pair with it: a replay onto it SHALL be refused as a mismatch, with nothing applied, rather than accepted and recovering a document that lacks every edit made before the enable. The test SHALL be exact rather than a count of edits, since no counter covers every kind of edit, and SHALL NOT refuse an unedited document whose snapshot this build would not write byte for byte, such as a load of an older format minor. A snapshot written after the enable SHALL pair with the journal from that point, as any snapshot does. A document that was never serialized SHALL still seed a journal that names no snapshot.

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

#### Scenario: A journal begun on a document edited since its load is refused against that load
- **GIVEN** a document loaded from a snapshot and edited while undo was off
- **WHEN** undo is enabled, further edits are made, and the journal from the start is replayed onto the snapshot it was loaded from
- **THEN** the replay is refused as a snapshot mismatch and nothing is applied

#### Scenario: A save after the enable pairs with the journal from there
- **GIVEN** a document edited while undo was off, then enabled
- **WHEN** it is saved, edited again, and the journal from the save's index is replayed onto that save
- **THEN** the replay is accepted and the recovered document holds the edits made before the enable and after it

#### Scenario: An unedited load still pairs, at any minor this build reads
- **WHEN** a snapshot written at this build's minor, or at an older one, is loaded, undo is enabled with no edit in between, edits are made, and the journal is replayed onto that snapshot
- **THEN** the replay is accepted and applies every step
