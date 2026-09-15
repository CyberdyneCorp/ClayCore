## ADDED Requirements

### Requirement: Independent sample-bound accumulators preserve exact maxima
The neighboring-sample bound reduction SHALL partition its comparisons into a fixed number of independent floating-point accumulators while preserving the existing maximum absolute forward-neighbor difference.

#### Scenario: Every sample coordinate contributes
- GIVEN a stored brick with a differing sample at any lattice coordinate
- WHEN its bound is measured
- THEN every in-block forward-neighbor pair SHALL be considered
- AND partial row tails SHALL be included
- AND the resulting bound SHALL match the scalar neighbor oracle bit for bit

#### Scenario: Non-finite and signed-zero samples
- GIVEN samples containing signed zeros, subnormals, finite extremes, infinities or NaNs
- WHEN absolute differences are accumulated
- THEN NaN differences SHALL leave the non-NaN maximum unchanged
- AND all non-NaN maximum bits SHALL match the scalar reduction

#### Scenario: Bounded scratch storage
- GIVEN any number of materialized bricks
- WHEN each block's bound is measured
- THEN the reduction SHALL require only constant stack storage and no heap allocations
