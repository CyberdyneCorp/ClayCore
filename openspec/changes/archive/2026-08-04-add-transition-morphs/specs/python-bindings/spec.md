# python-bindings — transitions from Python

Delta for `add-transition-morphs`.

## ADDED Requirements

### Requirement: Transition combine modes from Python
`clay.Op` SHALL expose `TRANSITION_LINEAR` and `TRANSITION_RADIAL`, parameterized by a `transition=` argument taking `clay.TransitionLinear(a, b, ease=…)` or `clay.TransitionRadial(r0, r1, ease=…)`. Using a transition op without its parameters SHALL raise a clear error, and the parameters SHALL survive a `.clayspace` round trip.

#### Scenario: Morph between two shapes
- **WHEN** a script adds a second primitive with `op=clay.Op.TRANSITION_LINEAR` and a segment spanning the scene
- **THEN** the document evaluates to the first shape at one end of the segment and the second at the other

#### Scenario: Missing parameters are rejected
- **WHEN** a transition op is used without a `transition=` argument
- **THEN** a `ValueError` names the missing argument instead of silently producing a degenerate morph
