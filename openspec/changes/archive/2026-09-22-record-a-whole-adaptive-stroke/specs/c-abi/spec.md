## ADDED Requirements

### Requirement: A host records a whole adaptive stroke as one undo step
The C ABI SHALL expose `clay_dynamic_sculptor_apply_stroke_recorded` and `clay_dynamic_sculptor_apply_preset_recorded`, taking the arguments of `clay_dynamic_sculptor_apply_stroke` and `clay_dynamic_sculptor_apply_preset` plus a `clay_dynamic_delta*` record, and capturing the whole stroke into that record as one replayable step. `clay_dynamic_sculptor_apply_stroke` and `clay_dynamic_sculptor_apply_preset` SHALL keep their signatures and behaviour.

A NULL record SHALL behave exactly as the unrecorded call. A non-empty record whose end is not the surface's current state SHALL be refused with `CLAY_ERROR_SNAPSHOT_MISMATCH`, applying nothing, reporting zero applied and leaving the record unchanged. Every `CLAY_ERROR_INVALID_ARGUMENT` refusal — a null handle, a malformed descriptor or sample, a short report, a Layer brush, a bad alpha — SHALL be decided before the mark, so a malformed call is never reported as retryable, and SHALL also leave the record unchanged.

A record whose end is the current state SHALL be continued, so one `clay_dynamic_delta_revert` undoes every stamp and stroke captured into it.

The header SHALL state the measured cost of recording beside the calls and what they do not promise, and the stroke calls' header, the library reference and the brush reference SHALL no longer say that a stroke can only be recorded one stamp at a time.

#### Scenario: A recorded ABI stroke reverts and applies exactly
- **WHEN** a host strokes an adaptive surface through `clay_dynamic_sculptor_apply_stroke_recorded`, then calls `clay_dynamic_delta_revert` and `clay_dynamic_delta_apply`
- **THEN** the surface after the stroke equals the one the unrecorded call produces on an identical surface, the export after the revert equals the pre-stroke export, the export after the apply equals the post-stroke export, and `clay_dynamic_surface_validate` passes at both ends

#### Scenario: A stale record is a retryable mismatch
- **WHEN** a host records a stroke, makes an unrecorded stamp, and strokes again into the same record
- **THEN** the call returns `CLAY_ERROR_SNAPSHOT_MISMATCH` with zero applied, the surface revision and export are unchanged, and the record's statistics are unchanged

#### Scenario: A malformed call wins over a stale record
- **WHEN** a host strokes with a Layer brush, or with a short report, into a record that no longer matches the surface
- **THEN** the call returns `CLAY_ERROR_INVALID_ARGUMENT` and the record's statistics are unchanged

#### Scenario: A NULL record is the unrecorded stroke
- **WHEN** a host calls `clay_dynamic_sculptor_apply_stroke_recorded` with a NULL record
- **THEN** the surface and report equal those of `clay_dynamic_sculptor_apply_stroke` with the same arguments
