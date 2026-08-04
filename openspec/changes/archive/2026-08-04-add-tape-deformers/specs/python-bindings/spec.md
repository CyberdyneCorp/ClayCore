# python-bindings — deformers from Python

Delta for `add-tape-deformers`.

## ADDED Requirements

### Requirement: Deformer modifiers on primitives
Primitives SHALL expose chainable deformer modifiers — `.twist(k)`, `.bend(k)`, `.taper(y0, y1, s0, s1, ease=…)`, and `.displace(amplitude, frequency)` — matching the `docs/05` §10 sample, applied in call order. Deformers SHALL be inspectable on the primitive and SHALL survive a `.clayspace` round trip. Constructs still absent from the tape (`wrap_around`, the two-subtree transitions) SHALL raise a clear error rather than silently doing nothing.

#### Scenario: Twisted primitive from the sample
- **WHEN** a script adds `clay.Box(size=(0.4, 0.4, 0.4)).twist(1.2)` with `op=clay.Op.SUBTRACT`
- **THEN** the document evaluates with the twist applied, and the same document authored through the C++ API yields the same field

#### Scenario: Chained deformers keep order
- **WHEN** a primitive is built as `.twist(1.0).taper(...)` and again as `.taper(...).twist(1.0)`
- **THEN** the two documents evaluate to different fields, each matching its authoring order
