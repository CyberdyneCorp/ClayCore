# python-bindings — elongate_axis

Delta for `add-elongate-axis-opcode`.

## ADDED Requirements

### Requirement: elongate_axis as a chainable modifier
`Prim.elongate_axis(h)` SHALL append a per-axis elongation, taking the half-extents. It SHALL compose in call order and survive a `.clayspace` round trip.

#### Scenario: Stretching an asymmetric primitive from Python
- **WHEN** a script elongates a cone per axis
- **THEN** the document evaluates a stretched field and no exception is raised

#### Scenario: Negative extents are refused
- **WHEN** any component of `h` is negative
- **THEN** the call raises
