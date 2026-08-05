# python-bindings — pose_line

Delta for `add-pose-line-regions`.

## ADDED Requirements

### Requirement: pose_line as a chainable modifier
`Prim.pose_line(a, b, axis, angle, ease=0)` SHALL append a line-gradient pose, compose in call order, and survive a `.clayspace` round trip.

#### Scenario: Posing along a limb from Python
- **WHEN** a script poses a capsule from one end to the other
- **THEN** the anchor end is unmoved and the far end is rotated

#### Scenario: A degenerate segment is refused
- **WHEN** the anchor and end coincide
- **THEN** the call raises, since the ramp would divide by zero
