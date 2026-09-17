## ADDED Requirements

### Requirement: An adaptive stroke records from Python
`DynamicSculptor.apply_stroke` and `DynamicSculptor.apply_preset` SHALL accept a `record` argument, a `TopologyDelta`, capturing the whole stroke as one replayable step with the same semantics as the C recorded stroke calls. A record that does not end where the surface is SHALL raise `ValueError` and apply nothing. The binding parity gate reads members rather than keyword arguments, so the pairing of `record` with `clay_dynamic_sculptor_apply_stroke_recorded` and `clay_dynamic_sculptor_apply_preset_recorded` SHALL be held by tests on both sides.

#### Scenario: A recorded Python stroke undoes and redoes to the exports
- **WHEN** a script applies a stroke with `record=`, reverts the record and applies it again
- **THEN** `to_mesh` equals the pre-stroke export after the revert and the post-stroke export after the apply, and `validate()` passes at both ends

#### Scenario: A stale record raises and applies nothing
- **WHEN** a script records a stroke, stamps without the record, and strokes again with the same record
- **THEN** the call raises `ValueError` and the surface's revisions are unchanged
