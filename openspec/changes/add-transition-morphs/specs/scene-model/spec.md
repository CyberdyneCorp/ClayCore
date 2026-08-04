# scene-model — non-local combine modes

Delta for `add-transition-morphs`.

## ADDED Requirements

### Requirement: Non-local combine modes report infinite influence
A combine mode whose weight is non-zero arbitrarily far from both operands SHALL report an infinite influence bound, so per-brick culling never drops it. Transition morphs are such modes: the linear weight is non-zero over a half-space and the radial weight past a radius. This preserves the blend-locality guarantee by refusing to claim locality that does not exist, rather than by silently corrupting culled bricks.

#### Scenario: Transition item is never culled
- **WHEN** an item combined with a transition mode is compiled for a brick far from both operands
- **THEN** the item still appears in the culled tape, and the culled result is band-clamp identical to the full tape

#### Scenario: Locality is preserved for rigid blends alongside transitions
- **WHEN** a scene mixes transition items with ordinary smooth-blend items
- **THEN** the smooth-blend items are still culled where their influence bounds do not reach the brick
