## ADDED Requirements

### Requirement: A host can resolve a stroke as its samples arrive
The C ABI SHALL expose an opaque `clay_stroke_tx` over `brush::StrokeTransaction`. `clay_stroke_tx_begin` takes a `clay_stroke_preset`, validated as `clay_stroke_resolve_full` validates it. `clay_stroke_tx_append` takes `clay_stroke_sample_full` samples and reports how many stamps the stroke grew by and the first stamp the append revised. `clay_stroke_tx_end` settles every stamp. `clay_stroke_tx_stamps` follows the size-query pattern, with `CLAY_ERROR_BUFFER_TOO_SMALL` for a short buffer. `clay_stroke_tx_status_get` fills a `clay_stroke_tx_status` descriptor bounded by its `struct_size`. After `clay_stroke_tx_end`, an append SHALL be refused with `CLAY_ERROR_INVALID_ARGUMENT` and SHALL append nothing.

#### Scenario: Batching does not change the stroke
- **WHEN** one path is appended in batches of 1, 5, 40 and an uneven schedule and the session is ended
- **THEN** `clay_stroke_tx_stamps` returns the bytes `clay_stroke_resolve_full` returns for the whole path, for every preset field

#### Scenario: An ended stroke takes no samples
- **WHEN** a host appends after `clay_stroke_tx_end`
- **THEN** the call returns `CLAY_ERROR_INVALID_ARGUMENT` and the sample count is unchanged

### Requirement: Each stroke consumer can be fed by a session
`clay_layer_apply_stroke_tx`, `clay_voxel_apply_stroke_tx`, `clay_mask_apply_stroke_tx`, `clay_mesh_sculptor_apply_stroke_tx`, `clay_dynamic_sculptor_apply_stroke_tx` and `clay_multires_sculptor_apply_stroke_tx` SHALL each apply the session's settled stamps that have not been applied yet. The call after `clay_stroke_tx_end` SHALL apply the rest and close the gesture, and later calls SHALL apply nothing. A gesture applied this way SHALL leave its target as the whole-path call leaves it: the SDF node list and ids byte for byte, the voxel cells including their per-stamp dither, the mask values, and the mesh, adaptive and multiresolution positions.

The first consumer call SHALL bind the session to its target, its handle arguments, its scalars and its descriptors. Every later call SHALL decode its descriptors again and compare them, field by field, with what the bind decoded; padding and the declared `struct_size` are not compared. A later call naming a different target, kind, handle or scalar, or passing a descriptor that is NULL, malformed or decodes differently, SHALL be refused with `CLAY_ERROR_INVALID_ARGUMENT` and apply nothing.

One gesture SHALL be one undo step. The SDF, voxel and mask consumers hold the owning document's undo group open from the bind to the close. A mesh gesture records into the caller's delta record. An adaptive gesture continues one `clay_dynamic_delta` across calls and refuses a call whose record the surface has moved past with `CLAY_ERROR_SNAPSHOT_MISMATCH`, taking no stamps from the session. A multiresolution gesture continues one `clay_multires_delta` across calls, capturing both the base half and, with an active sculpt pass, the pass half, so the record equals the one `clay_multires_sculptor_apply_stroke_recorded` makes for the whole path; it refuses a call whose record the hierarchy no longer accepts (another hierarchy, a changed level structure, or a different pass active while the record holds one) with `CLAY_ERROR_SNAPSHOT_MISMATCH`, taking no stamps from the session. A pass made active mid-gesture while the record holds only base entries joins it, and the record then holds both halves. For both, the record is part of the binding, NULL records nothing and stamps what a recorded gesture stamps, and a call that applies nothing (after the close in particular) leaves the record and its binding untouched. Destroying a session whose gesture is open SHALL close the gesture without applying the stamps it held back.

The whole-path calls SHALL be unchanged.

#### Scenario: A layer gesture is one step and the whole-path node list
- **WHEN** a stroke is fed to `clay_layer_apply_stroke_tx` in batches of 1, 7 and 25 with a call after each append
- **THEN** the saved document is byte-identical to the one `clay_layer_apply_stroke` produces, the undo depth is 1, and one undo removes the gesture

#### Scenario: A sculptor gesture keeps its carried region across calls
- **WHEN** a grab is fed to `clay_mesh_sculptor_apply_stroke_tx` in batches, with and without a declared world frame
- **THEN** the positions equal `clay_mesh_sculptor_apply_stroke`'s byte for byte, and one revert of the delta record restores the mesh

#### Scenario: A start taper applies at lift
- **WHEN** a stroke with `taper_start > 0` is fed to a consumer
- **THEN** nothing is applied before `clay_stroke_tx_end`, and the whole stroke is applied by the call after it

#### Scenario: A refused adaptive call loses no stamps
- **WHEN** an unrecorded stamp moves the surface between two calls of a recorded adaptive gesture
- **THEN** the next call returns `CLAY_ERROR_SNAPSHOT_MISMATCH` and applies nothing, and once the record is cleared the following call applies the held stamps

#### Scenario: A multires gesture fed in batches is one record
- **WHEN** a stroke is fed to `clay_multires_sculptor_apply_stroke_tx` in batches with one `clay_multires_delta`, on a hierarchy with and without an active sculpt pass
- **THEN** the positions equal `clay_multires_sculptor_apply_stroke_recorded`'s, the record's entry counts, levels and encoded size equal that call's record, and one `clay_multires_delta_revert` restores every level bit for bit

#### Scenario: A refused multires call loses no stamps
- **WHEN** another sculpt pass is made active between two calls of a recorded multires gesture
- **THEN** the next call returns `CLAY_ERROR_SNAPSHOT_MISMATCH` and applies nothing, and once the first pass is active again the following call applies the held stamps and the gesture ends where the whole-path stroke does

#### Scenario: A call after the close leaves a multires record alone
- **WHEN** a recorded multires gesture has closed, the record is reverted, the top level is removed and added back, and the host makes one more consumer call
- **THEN** that call returns `CLAY_OK` and applies nothing, and the record is still refused by `clay_multires_delta_apply` and `_revert` with `CLAY_ERROR_SNAPSHOT_MISMATCH`

#### Scenario: The binding is checked
- **WHEN** a bound session is passed another layer, another item, a mask it was not bound with, another scalar, another kind of target, a NULL brush descriptor, a brush descriptor with another strength, another topology, another frame, or another record than the one it was bound with (NULL included)
- **THEN** each call returns `CLAY_ERROR_INVALID_ARGUMENT` and applies nothing
