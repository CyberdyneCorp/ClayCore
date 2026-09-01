# scene-model

## MODIFIED Requirements

### Requirement: Non-local combine modes report infinite influence
A combine mode whose weight is non-zero arbitrarily far from both operands SHALL report an infinite influence bound, so per-brick culling never drops it. Transition morphs are such modes: the linear weight is non-zero over a half-space and the radial weight past a radius. This preserves the blend-locality guarantee by refusing to claim locality that does not exist, rather than by silently corrupting culled bricks.

An INTERSECT is not such a mode and SHALL NOT report an infinite influence bound. `max(acc, item)` can only take material away, and what it takes away is inside what the layer already occupies, so an intersect's influence SHALL be the LAYER's own extent — the union of its visible items' geometry bounds — and infinite only when that union is itself unbounded. An intersect whose non-locality has a second cause, such as an infinite grid repeat or an unbounded primitive, SHALL remain infinite: the weaker answer does not win.

CULLABILITY IS A SEPARATE QUESTION and SHALL NOT change. Every non-local op, intersect included, SHALL remain ineligible for per-brick culling, so an intersect still appears in every brick's tape. The finite bound answers only which bricks an edit dirties.

#### Scenario: Transition item is never culled
- **WHEN** an item combined with a transition mode is compiled for a brick far from both operands
- **THEN** the item still appears in the culled tape, and the culled result is band-clamp identical to the full tape

#### Scenario: Locality is preserved for rigid blends alongside transitions
- **WHEN** a scene mixes transition items with ordinary smooth-blend items
- **THEN** the smooth-blend items are still culled where their influence bounds do not reach the brick

#### Scenario: An intersect reports its layer's extent
- **WHEN** an item combined with intersect is queried for its influence bound in a layer whose other items reach further than it does
- **THEN** the bound is finite and contains those other items, and the item is still reported as ineligible for culling

#### Scenario: An intersect is still never culled
- **WHEN** an item combined with intersect is compiled for a brick its own geometry does not reach
- **THEN** the item still appears in the culled tape, and the culled result is band-clamp identical to the full tape

#### Scenario: An unbounded layer bounds nothing
- **WHEN** an intersect sits in a layer that also holds a primitive with no finite extent
- **THEN** its influence bound is infinite

#### Scenario: A second cause of non-locality wins
- **WHEN** an item combines with intersect AND repeats on an infinite grid
- **THEN** its influence bound is infinite

## ADDED Requirements

### Requirement: A candidate influence bound is ranked on measurement

A change that TIGHTENS an influence bound SHALL be justified by a measurement that distinguishes the bound it ships from the tighter one it rejects, not by reasoning alone. The property test SHALL sample densely enough that a bound which is too small produces a non-zero drift.

The sample count is a property of how RARE a violation is rather than of how hard the document is. A local item's bound is its own geometry and a violation there is dense; a non-local item's violation can be one sample in ten thousand, so the non-local cases SHALL sample at a rate that finds it every run.

#### Scenario: A too-small bound is caught
- **WHEN** the property test runs against an intersect's own geometry bound instead of the bound the engine reports
- **THEN** it reports a non-zero band-clamped drift outside that box

#### Scenario: The shipped bound holds
- **WHEN** the same test runs against the bound the engine reports
- **THEN** the drift is exactly zero over the same samples
