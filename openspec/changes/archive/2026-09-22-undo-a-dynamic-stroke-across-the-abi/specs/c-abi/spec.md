## ADDED Requirements

### Requirement: Adaptive-surface undo across the ABI
The C ABI SHALL expose an opaque record handle for an adaptive gesture, `clay_dynamic_delta`, with create, destroy and clear calls. It SHALL expose a stamp entry point that accumulates into a record, `clay_dynamic_sculptor_stamp_recorded`, while `clay_dynamic_sculptor_stamp` stays unchanged. Revert and apply SHALL take the SCULPTOR, so replay keeps its index and chunk stream in step, and SHALL NOT require `clay_dynamic_sculptor_rebuild_index`.

A replay onto a surface not at the record's end state SHALL return `CLAY_ERROR_SNAPSHOT_MISMATCH` and write nothing. That includes a replay out of last-in first-out order, one after an unrecorded change, and one against another surface. A record SHALL be bound to the surface handle it was captured on. A deserialized surface is a different handle, so no record replays onto it.

A statistics call SHALL report the entries per element kind, `resident_bytes` (what the record holds in memory, capacities included, for host undo budgeting) and `encoded_bytes`. `encoded_bytes` SHALL equal exactly what the serialize call writes, and SHALL be a fixed function of the entry counts, independent of platform and allocator. Serialize SHALL follow the size-query pattern and report a short buffer as `CLAY_ERROR_BUFFER_TOO_SMALL`. Deserialize SHALL refuse truncated or hostile bytes before allocating, and SHALL report a newer format version as `CLAY_ERROR_FORWARD_VERSION`.

The header SHALL state what the calls do not promise:
- `clay_dynamic_surface_serialize` output is not byte-identical after an undo;
- dead slots stay, and `dead_slots` grows;
- `rebuild_index` after an undo clears the dirty set and renumbers chunks;
- a serialized record is for spilling within the handle's lifetime, not for crash recovery.

#### Scenario: Undo restores the surface exactly
- **WHEN** a host records an adaptive stroke that splits, collapses and flips, then reverts it
- **THEN** `clay_dynamic_surface_to_mesh` returns positions, normals and indices byte-identical to the pre-stroke export, and `clay_dynamic_surface_validate` reports ok

#### Scenario: Redo reproduces the stroke
- **WHEN** the reverted record is applied
- **THEN** the export is byte-identical to the post-stroke export and validation reports ok

#### Scenario: A sequence undoes in reverse
- **WHEN** N strokes are recorded into N records and reverted from last to first
- **THEN** after each revert the export equals the export taken before that stroke, and applying them again in order reproduces each post-stroke export

#### Scenario: The byte cost is a count
- **WHEN** a host reads the statistics of a captured record
- **THEN** `encoded_bytes` equals the documented formula over the reported entry counts and equals the size the serialize call returns, `resident_bytes` is at least the entries times their in-memory size, and a cleared record reports zero entries

#### Scenario: A replay against the wrong state is retryable, not malformed
- **WHEN** a host reverts an older record while a newer one is still applied
- **THEN** the call returns `CLAY_ERROR_SNAPSHOT_MISMATCH`, the surface revision is unchanged, and reverting the newer record first and then the older one succeeds

#### Scenario: A spilled record replays
- **WHEN** a record is serialized, destroyed, deserialized and reverted against the surface handle it was captured on
- **THEN** the result equals reverting the original record
