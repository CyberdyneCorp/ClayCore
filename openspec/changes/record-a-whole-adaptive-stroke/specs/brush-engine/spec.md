## ADDED Requirements

### Requirement: An adaptive stroke records into a replayable gesture
The brush engine SHALL offer a recorded sibling of the adaptive stroke consumer that captures the whole stroke into a replayable gesture — the topology delta together with the surface marks at both ends — so that one stroke, with the stroke's own drag anchoring and remesh schedule, is one undo step the sculptor's guarded replay accepts.

The surface mark SHALL be checked ONCE, before the first stamp: a non-empty record whose end is not the surface's current state SHALL be refused with nothing stamped, no revision advanced and the record unchanged. The refusals of the unrecorded consumer (a Layer verb, a request to defer normals, no stamps) SHALL be decided before the mark check and SHALL also leave the record unchanged. A mark check between stamps SHALL NOT be added while every write inside the stroke is a sculptor stamp into the same record, because such a check cannot fire.

A non-empty record whose end is the current state SHALL be continued, so stamps and strokes captured in sequence revert and apply as one step.

The surface a recorded stroke produces SHALL be bit-identical to the unrecorded stroke's with the same inputs, and the record SHALL be the same size as the one the same stamps produce when captured one by one with the stroke's rules.

#### Scenario: A recorded stroke undoes and redoes exactly
- **WHEN** a Draw, Clay, Smooth, Flatten, Grab or Snakehook stroke is applied through the recorded consumer with the relax pass on, then the record is reverted and applied through the sculptor
- **THEN** after the revert the export and the stored normals of every live element equal the pre-stroke surface bit for bit, after the apply they equal the post-stroke surface, the surface validates at both ends, and the sculptor's index covers every live face

#### Scenario: A Snakehook whose anchor dies records exactly
- **WHEN** a recorded Snakehook stroke runs on a coarse detail where the remesher retires the dragged vertex at least once
- **THEN** reverting and applying the record reproduce the pre- and post-stroke surfaces exactly

#### Scenario: Recording does not change the stroke
- **WHEN** the same stamps are applied to identical surfaces by the recorded and the unrecorded consumer
- **THEN** the surfaces are bit-identical, the applied counts and summaries are equal, and the record's encoded size equals that of the same stamps captured one by one with the stroke's rules

#### Scenario: A stale record refuses the whole stroke
- **WHEN** a record captured on a surface is followed by an unrecorded stamp, and a stroke is then applied into that record
- **THEN** the call reports the mismatch, no stamp is applied, and the record's size and both marks are unchanged

#### Scenario: A refused stroke leaves the record untouched
- **WHEN** a Layer stroke, a stroke asking to defer normals, or an empty stroke is applied into a non-empty record
- **THEN** the call applies nothing and the record's size and both marks are unchanged

#### Scenario: Strokes accumulate into one step
- **WHEN** a recorded stamp and two recorded strokes are captured into one record and the record is reverted once
- **THEN** the surface equals the one before the stamp
