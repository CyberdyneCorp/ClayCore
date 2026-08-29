# brick-cache — a seed's pad holds still

Delta for `hold-the-cull-pad-still`.

## ADDED Requirements

### Requirement: The cull pad a seed is keyed by is piecewise constant

A stored brick value is only reusable under the cull pad it was computed with,
and that gate SHALL remain exact: a value continued from a differently culled
tape is a different field, not a rounding difference.

Because the gate is exact, the pad SHALL NOT change on every append. It SHALL
be piecewise constant in the node count, changing only at a bounded number of
stated steps across the range where its underlying fit varies. Adding one node
to a document SHALL leave the pad unchanged except when that node crosses a
step.

Where the pad is quantised to achieve this, it SHALL be rounded so that the
value used is never SMALLER than the fit it replaces. A larger pad keeps more
items in a brick's culled tape, and the band-clamped result cannot be changed by
keeping an item that could not have changed it — so rounding up is conservative
in the sense every bound in this engine is stated in, while rounding down would
not be.

A step boundary SHALL cost what a pad change costs — one full refill of the
bricks it reaches — and that is correct, because at a step the pad really did
change. What SHALL NOT happen is paying that on every dab.

#### Scenario: An append does not move the pad
- **GIVEN** a document whose node count sits between a pair of steps
- **WHEN** a node is appended
- **THEN** the cull pad is unchanged, and a brick refilled before the append is answered from its stored value rather than walked again

#### Scenario: A stroke keeps the resume across the whole range
- **WHEN** a stroke of equal dabs is added to smooth-blended documents spanning the range over which the pad's fit varies
- **THEN** the bricks answered from a stored value per dab do not fall to zero at any document size

#### Scenario: The quantised pad is never smaller than the fit
- **WHEN** the pad is resolved at any node count
- **THEN** the value used is greater than or equal to the unquantised fit at that node count

#### Scenario: Band-clamped results are unchanged
- **WHEN** a document is evaluated per brick under the quantised pad and under the unquantised fit
- **THEN** the band-clamped values are identical

#### Scenario: A step costs one refill, not one per dab
- **WHEN** a stroke crosses a step boundary
- **THEN** the bricks it reaches are walked again once, and the dabs on either side of the step are answered from stored values
