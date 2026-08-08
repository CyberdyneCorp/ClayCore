# brush-engine — stroke strength on relief

Delta for `add-stroke-strength-to-relief`.

## ADDED Requirements

### Requirement: A stamp's strength scales an amplitude, where the op has one
Turning stamps into edit-list items SHALL apply a stamp's strength to the item's amplitude for the ops whose `blend.k` IS an amplitude — relief and incise — so that the stroke engine's pressure-strength channel and its accumulation mode reach an SDF layer.

For every other op `blend.k` is a radius, a depth or a half-thickness. Scaling those by a stroke's strength would change the SHAPE rather than the amount, differently per op and silently, so those ops SHALL continue to ignore strength. A boolean has no partial application: a union at half strength is not a smaller union.

#### Scenario: Buildup accumulates past clamped
- **WHEN** the same dense relief stroke is applied under buildup and under clamped accumulation
- **THEN** the buildup pass moves the surface further

#### Scenario: A lighter touch deposits less
- **WHEN** a relief stroke is applied with reduced pressure strength
- **THEN** the surface moves less than at full strength

#### Scenario: A boolean stroke is unaffected
- **WHEN** an add stroke is applied under buildup and under clamped accumulation
- **THEN** the two results are identical
