## ADDED Requirements

### Requirement: A fixture states the regime it measures, and asserts it

A case measuring the brick refill SHALL state the regime its fixture puts the
cache in, and SHALL assert that regime at both ends rather than assume it.

The refill's cost per brick depends on whether the brick straddles the surface
or lies inside the solid, and that is decided by the fixture's geometry and its
resolution together, not by the verb. Every SDF brick case before this one
stamped radius-0.12 spheres into empty space at `voxel_size` 0.05, where one
brick spans 0.4 units: the dab is smaller than a brick, so every brick it
dirties straddles it and none has an interior. The suite therefore measured one
regime while naming the verb, and a change to what an interior brick costs moved
nothing in it.

A case SHALL assert that its filled fixture stores some surface AND that it is
not mostly surface. The first catches a cache that stored nothing — which is
what a refused submit leaves behind, and which reads as a healthy run. The
second catches a fixture that has lost its interior and is measuring a regime
another case already covers.

#### Scenario: A fixture with no interior
- **WHEN** a case's filled fixture is mostly surface bricks
- **THEN** the case fails, because it measures a regime an existing case covers

#### Scenario: A cache that stored nothing
- **WHEN** a case's fill leaves the cache with no surface bricks at all
- **THEN** the case fails rather than reporting the timings it took against it

#### Scenario: Both regimes are measured
- **WHEN** a change alters what an interior brick costs the refill
- **THEN** a detail-resolution case moves and a blockout-resolution case does not
- **AND** neither case alone would have shown it

### Requirement: A submit whose result is discarded is not a measurement

A case driving `clay_brick_cache_submit` SHALL pass `out_results` or
`out_accepted` and SHALL check what came back.

The ABI refuses a submit that asks for neither, because with neither a caller
cannot tell an accepted brick from a stale one. A case that passes neither and
discards the return value evaluates its bricks — which is real, timed, dominant
work — into a cache that stays empty for the length of the run, and nothing in
it measures storage, classification, or a refill reading a populated cache.

#### Scenario: The cache is asked what it accepted
- **WHEN** a case submits evaluated bricks
- **THEN** it asks how many were accepted and fails if that is not all of them

### Requirement: A new suite takes the warm end of the run

A device suite whose baseline does not yet exist SHALL be added at the END of
the run, in its own session, and SHALL NOT be inserted ahead of an existing one.

Run order is measured at 2.7x on a gate whose tolerance is 1.4x, and every
committed baseline was taken in the existing order, so inserting a suite earlier
re-baselines everything below it. Taking the warm end can only make the new
suite's own figures pessimistic, which is the safe direction for numbers nothing
is yet compared against.

A suite SHALL take its own SESSION, and not merely its own bundle, when what it
costs the run is heat rather than memory. A process boundary returns a memory
high-water mark and does not return temperature; only a session boundary with a
cooldown ahead of it starts a suite cold.

#### Scenario: A suite that heats the device
- **WHEN** a case added to an existing bundle takes that session from nominal to serious
- **THEN** it is moved to its own session rather than merely its own bundle

#### Scenario: An existing baseline keeps its position
- **WHEN** a suite is added to the gate
- **THEN** no existing case changes the position it is measured at
