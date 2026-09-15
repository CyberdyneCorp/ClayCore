## ADDED Requirements

### Requirement: Initial materialization adopts filled sample storage
Materializing source bricks into an empty volume SHALL retain the first filled sample block without allocating and copying a second complete sample payload.

#### Scenario: Full source priming
- GIVEN an empty working lattice and a fill covering all its bricks
- WHEN the source is materialized
- THEN sample values, brick order, bounds and reported added coordinates SHALL match the existing materialization semantics
- AND total allocation requests including bookkeeping SHALL remain below two complete sample payloads in the allocation regression fixture

#### Scenario: Incremental and repeated fills
- GIVEN a partially materialized volume with existing samples
- WHEN a later region is materialized or the same region is requested again
- THEN existing samples SHALL remain unchanged
- AND only newly requested bricks SHALL invoke source fills and appear in the added-coordinate output
- AND each fill callback SHALL observe only previously materialized samples
