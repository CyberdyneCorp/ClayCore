# python-bindings — the ramped bends

Delta for `add-bend-opcodes`.

## ADDED Requirements

### Requirement: bend_linear and bend_radial as chainable modifiers
`Prim.bend_linear(a, b, v, ease=0)` and `Prim.bend_radial(r0, r1, dz, ease=0)` SHALL append the corresponding deformer, compose in call order, and survive a `.clayspace` round trip.

#### Scenario: Ramping from Python
- **WHEN** a script adds a primitive with `.bend_linear(...)` or `.bend_radial(...)`
- **THEN** the document evaluates a displaced field and no exception is raised

#### Scenario: A degenerate span is refused
- **WHEN** the two ramp endpoints coincide, or `r0` equals `r1`
- **THEN** the call raises, since the ramp would divide by zero
