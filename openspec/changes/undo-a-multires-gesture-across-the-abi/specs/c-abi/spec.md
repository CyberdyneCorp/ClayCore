## ADDED Requirements

### Requirement: Multiresolution undo across the ABI
The C ABI SHALL expose an opaque record handle for one multiresolution gesture, `clay_multires_delta`, with create, destroy and clear calls. The record SHALL hold both halves a gesture can write: the base (cage positions and each level's own detail coefficients) and one sculpt pass (its coefficients and mask weights).

A host SHALL be able to capture into a record through `clay_multires_sculptor_stamp_recorded`, `clay_multires_sculptor_apply_stroke_recorded` and `clay_multires_sculpt_layer_stroke_commit_into`, while `clay_multires_sculptor_stamp`, `clay_multires_sculptor_apply_stroke` and `clay_multires_sculpt_layer_stroke_commit` stay unchanged. A null record SHALL behave exactly as the unrecorded call. The plain sculptor's recorded calls SHALL capture whichever half the stamp wrote, including the active sculpt pass.

`clay_multires_delta_revert` and `clay_multires_delta_apply` SHALL take the hierarchy, restore every recorded value to its value before or after the gesture, be idempotent, and mark the patches that moved so `clay_multires_dirty_blocks` reports them. A record SHALL be bound to the hierarchy and level structure it was captured on. A replay onto another hierarchy, onto this one after a level was added, removed or refined, or holding a pass the stack no longer has, SHALL return `CLAY_ERROR_SNAPSHOT_MISMATCH` and write nothing. Continuing a non-empty record onto another hierarchy or structure, or while a different pass is active, SHALL return `CLAY_ERROR_SNAPSHOT_MISMATCH` and stamp nothing, after every `CLAY_ERROR_INVALID_ARGUMENT` the unrecorded call would return.

A statistics call SHALL report the entries per half and kind, the pass the layer half belongs to, `resident_bytes` for budgeting, and `encoded_bytes`, which SHALL equal exactly what serialize writes and be a fixed function of the entry counts. A levels call SHALL follow the size-query pattern. Serialize SHALL follow the size-query pattern and report a short buffer as `CLAY_ERROR_BUFFER_TOO_SMALL`. Deserialize SHALL refuse truncated, trailing or hostile bytes before allocating, and SHALL report a newer format version as `CLAY_ERROR_FORWARD_VERSION`.

The header SHALL state what the calls do not promise: a record does not survive a level change or a decode of the hierarchy's own bytes; overlapping records are replayed last in, first out by the host; a replay during an open sculpt-layer stroke is not refused; the memory ledger does not count records; serialized records are for spilling within the process.

#### Scenario: A recorded stroke undoes and redoes bit for bit
- **WHEN** a host records a stroke through the plain sculptor, then reverts, reverts again, applies and reverts
- **THEN** the base checksum, the sculpt-layer checksum and every level's evaluated positions equal the pre-stroke values after each revert and the post-stroke values after the apply

#### Scenario: Every way a gesture lands is covered
- **WHEN** the recorded gesture is at level 0, crosses a regionally refined region's rim, is mirrored into one record, writes the active pass through the plain sculptor, or is a sculpt-layer transaction committed into a record
- **THEN** revert and apply restore the pre- and post-gesture state exactly

#### Scenario: Another hierarchy or a changed structure is refused
- **WHEN** a record is replayed onto a twin built from the same cage with the same levels, or onto its own hierarchy after the top level was removed and added back
- **THEN** the call returns `CLAY_ERROR_SNAPSHOT_MISMATCH` and the hierarchy is unchanged

#### Scenario: The record follows the vertices reached
- **WHEN** one stamp and forty stamps are recorded on the same spot of two identical hierarchies
- **THEN** both records report the same entry counts and the same `encoded_bytes`

#### Scenario: A spilled record replays
- **WHEN** a record is serialized, destroyed, deserialized and replayed onto the hierarchy it was captured on
- **THEN** the result equals replaying the original, and `encoded_bytes` equals the documented formula and the serialize size

#### Scenario: A replay is visible to the block transport
- **WHEN** a host clears the dirty set and reverts a record holding a base half or a pass half
- **THEN** the evaluated revision advances and `clay_multires_dirty_block_count` is nonzero
