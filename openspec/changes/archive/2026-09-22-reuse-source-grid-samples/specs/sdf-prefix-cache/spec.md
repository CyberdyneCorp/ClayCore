## ADDED Requirements

### Requirement: Shared full-grid source samples
For eligible complete uncomposed source grids, the source field SHALL evaluate each unique lattice position once and reproduce the existing brick sample layout exactly.

#### Scenario: Shared boundaries
- GIVEN a complete source grid eligible for shared sampling
- WHEN its bricks are filled
- THEN each unique lattice coordinate SHALL be evaluated once in a single batch
- AND every brick sample SHALL match independent per-brick evaluation bit for bit
- AND materialized volume bytes and bounds SHALL remain identical

#### Scenario: Existing fallback paths
- GIVEN a partial, small, invalid or unrepresentable grid, or a composed prefix source
- WHEN source sampling is requested
- THEN the sharing helper SHALL decline before invoking the evaluator or writing output
- AND valid source calls SHALL retain their existing sampling and prefix-coverage behavior

#### Scenario: Bounded temporary storage
- GIVEN an eligible grid
- WHEN unique positions and values are allocated
- THEN their combined float count SHALL not exceed the existing duplicate-position scratch count
- AND dimension, coordinate and allocation arithmetic SHALL be checked before allocation
