# python-bindings — elongate

Delta for `add-elongate-opcode`.

## ADDED Requirements

### Requirement: elongate as a chainable modifier
`Prim.elongate(h)` SHALL append an elongation to the primitive's chain, taking the per-axis half-extent. It SHALL compose with the other modifiers in call order and survive a `.clayspace` round trip.

#### Scenario: Stretching from Python
- **WHEN** a script adds `clay.Sphere(r=0.5).elongate((1.0, 0, 0))`
- **THEN** the document evaluates a capsule-like field stretched along X

#### Scenario: Negative extents are refused
- **WHEN** any component of `h` is negative
- **THEN** the call raises, since a half-extent has no meaning below zero
