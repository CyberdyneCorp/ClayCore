# sdf-kernels — transitions reachable from a document

Delta for `add-transition-morphs`.

## ADDED Requirements

### Requirement: Transition morphs as combine modes
The tape SHALL provide `transition_linear` and `transition_radial` combine modes that mix the accumulated field with an item's field by a spatially varying weight (01 §2.5): the linear weight from the eased projection onto a segment a→b, the radial weight from the eased XZ radius between r0 and r1. Both SHALL mix color by the same weight, and both SHALL fold `cfi_transition` into the tape's tracked field info — a lerp of two distance fields is not a distance, so the safe step scale SHALL drop accordingly.

#### Scenario: Weight endpoints select each operand
- **WHEN** a transition_linear item is evaluated at the segment's start point and at its end point
- **THEN** the result equals the accumulated field at the start and the item's field at the end

#### Scenario: Transition matches the kernel weight
- **WHEN** a transition item is evaluated at arbitrary points
- **THEN** the result equals `mix(accumulated, item, ctransition_*_weight(p, ...))` for the same parameters

#### Scenario: Tracked step scale falls under a transition
- **WHEN** a tape containing a transition is compiled
- **THEN** its field info is non-exact, its safe step scale is below 1, and stepping by that scale never crosses the surface
