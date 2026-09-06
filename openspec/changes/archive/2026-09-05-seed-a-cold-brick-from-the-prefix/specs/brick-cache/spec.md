# brick-cache — what a cold brick may start from

Delta for `seed-a-cold-brick-from-the-prefix`.

## ADDED Requirements

### Requirement: A cold brick may start from a prefix only where the prefix stores it

A brick with no resident seed MAY be started from a cached prefix of its layer's
history, and SHALL be started from one only where that prefix stores EVERY sample
of the brick's window.

A sparse sampled field answers interpolation where it stores samples and a
conservative far bound where it does not. A suffix folded onto a far bound is
wrong by cells rather than by rounding, and nothing in the result says so. A
window that is not fully stored SHALL therefore take the ordinary full
evaluation — slower, and never wrong.

Where a prefix is used, the field it produces SHALL equal what the full
evaluation produces, within the band, to the tolerance the prefix's own sampling
declares.

#### Scenario: A covered window is seeded
- **WHEN** the prefix stores every sample of a cold brick's window
- **THEN** the brick is seeded from it and its values agree with the full evaluation

#### Scenario: An uncovered window is not
- **WHEN** the prefix stores only part of a cold brick's window
- **THEN** that brick takes the full evaluation, and its values agree with it

### Requirement: A prefix stays usable as its layer grows

Appending to a layer SHALL NOT retire a cached prefix of the roots before the
append.

The boundary a policy names moves with every append, so a lookup by the current
boundary would miss on every stamp of a stroke — which is when a cold brick is
most often reached. A consumer that can continue from any boundary SHALL be
offered the best one the cache holds, and SHALL evaluate the roots after it.

#### Scenario: A stroke keeps hitting
- **WHEN** items are appended to a layer after its prefix was built, and a cold window is then refilled
- **THEN** the prefix still serves it, and the result includes the appended items
