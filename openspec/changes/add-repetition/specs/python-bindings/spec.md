# python-bindings — repetition modifiers

Delta for `add-repetition`.

## ADDED Requirements

### Requirement: Repetition modifiers on primitives
Primitives SHALL expose chainable repetition modifiers: `.repeat_grid(spacing, counts=None)` — infinite when `counts` is omitted, finite otherwise — and `.repeat_radial(count, offset)`. Repetition SHALL survive a `.clayspace` round trip and compose with deformers.

#### Scenario: Finite array from Python
- **WHEN** a script adds `clay.Sphere(r=0.2).repeat_grid(spacing=1.0, counts=(2, 0, 0))`
- **THEN** the document contains copies at each cell of the range and none beyond it

#### Scenario: Radial array from Python
- **WHEN** a script adds a primitive with `.repeat_radial(count=6, offset=1.0)`
- **THEN** the field is periodic under rotation by one sixth of a turn about the axis
