# python-bindings — wrap_around

Delta for `add-wrap-around-opcode`.

## ADDED Requirements

### Requirement: wrap_around as a chainable modifier
`Prim.wrap_around(x0, x1)` SHALL append a wrap deformer to the primitive's chain, replacing the stub that raises. It SHALL compose with the other modifiers in call order and survive a `.clayspace` round trip.

#### Scenario: Wrapping from Python
- **WHEN** a script adds a primitive with `.wrap_around(x0, x1)`
- **THEN** the document evaluates a wrapped field and no exception is raised

#### Scenario: Degenerate interval is refused
- **WHEN** `x0` and `x1` are equal
- **THEN** the call raises rather than producing a zero-radius cylinder
