# scene-model — bounds for repeated items

Delta for `add-repetition`.

## ADDED Requirements

### Requirement: Influence bounds for repetition
A repeated item's influence bound SHALL cover every copy it produces: a finite grid sweeps the item's bound across its occupied cell range, and a radial array sweeps it into an annulus about the axis — both finite and therefore cullable. An infinite grid SHALL report infinite influence, since it produces copies arbitrarily far away.

#### Scenario: Finite array stays inside its bound
- **WHEN** the influence-bound property test runs on finite grid and radial array items
- **THEN** band-clamped field values outside the bound are bit-identical with and without the item, and per-brick culled tapes stay band-clamp identical

#### Scenario: Infinite grid is never culled
- **WHEN** an item with an infinite grid repetition is compiled for any brick
- **THEN** it appears in the culled tape and the culled result is band-clamp identical to the full tape
