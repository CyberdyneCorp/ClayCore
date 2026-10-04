## ADDED Requirements

### Requirement: A stroke under the pen says which of its stamps are final
`brush::StrokeTransaction` SHALL report how many of its leading stamps are settled, meaning no later append can change their position, radius, strength, deposit or rotation. A stamp SHALL count as settled only when the path holds at least two samples, its arc-length station lies on the path already received, it is outside the end taper at the current length, and the preset has no start taper. After `finish()` every stamp SHALL be settled, and `append` SHALL take no more samples. `along` is not part of the rule. It is a fraction of the whole path, and no consumer reads it.

A consumer that applies exactly the settled stamps, in order, as they settle SHALL apply the stamps the pure resolver gives for the whole path, bit for bit, however the samples were batched.

#### Scenario: Settled stamps are the finished stroke's
- **WHEN** a path is appended in batches of 1, 5, 40 and an uneven schedule, under every preset field (spacing, both tapers, steady, jitter, pressure and velocity response, both rotations, clamped accumulation)
- **THEN** after every append, each settled stamp equals the stamp at that index of the finished stroke, and after `finish()` the stamps equal `resolve_stroke` of the whole path, `along` included

#### Scenario: The end taper waits
- **WHEN** a stroke with an end taper is half received
- **THEN** its last stamps are tapered and not settled, and they settle at full width once the stroke moves past them

#### Scenario: A start taper holds everything
- **WHEN** a stroke has a start taper
- **THEN** none of its stamps settle before `finish()`

#### Scenario: Ink arrives under the pen
- **WHEN** a stroke without a start taper is half received
- **THEN** at least a quarter of its final stamps are already settled

### Requirement: A mesh stroke can be applied in pieces as one gesture
The fixed, multiresolution and adaptive mesh consumers SHALL each be available as a gesture that takes its stamps over several calls and keeps, between them, what a stroke carries: the first stamp, a grab's carried region, a snakehook's anchor, the multiresolution level record, and the deferred-normal flag. The whole-stroke functions SHALL be one such gesture fed once, so a stroke applied in pieces is bit-identical to the same stamps applied whole.

#### Scenario: A grab fed in pieces is the whole grab
- **WHEN** a grab's settled stamps are fed to one gesture over several calls
- **THEN** the mesh positions equal the whole-stroke call's, byte for byte, while the same batches fed to the whole-stroke call as separate strokes do not
