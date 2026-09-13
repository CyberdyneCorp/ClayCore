## ADDED Requirements

### Requirement: A cross-level neighbourhood is re-derived only when the level below has moved

The outside positions of a level's cross-level neighbourhood belong to the level
BELOW: they are the pure subdivision of that level's positions, so a stroke down
there moves them without anything the level above stores going stale. They SHALL
be re-derived whenever the level below's positions have changed since the last
time they were read, and SHALL NOT be re-derived when they have not.

Handing back an answer read before a change is forbidden: a reader completes its
boundary normals and frames from those positions and cannot distinguish a stale
answer from a current one.

The signal SHALL be the queue the level above already depends on. A level's
positions and the vertices it owes the level above are one fact, not two —
the neighbourhood's outside positions and the level's own subdivided positions
come from the same call on the same input — so a level's changed-vertex queue and
its positions revision SHALL move together through one door, and a writer SHALL
NOT be able to move one without the other. A revision maintained only where the
positions are ASSIGNED is insufficient and SHALL NOT be used: a stroke at level 0
is written into the cache's mesh by the brush and read out into the cage by the
hierarchy, so no assignment happens at that level and the level above would be
served a rim from before the stroke.

A write that RESTORES a level's positions to what its stored coefficients
reconstruct to SHALL NOT count as a change, because that is the value the current
revision already names.

The hierarchy SHALL report how many times a cross-level neighbourhood was asked
for and how many of those asks re-derived it, so the caching is observable as a
mechanism rather than inferred from values. A cache that has silently stopped
caching, and a cache that has silently stopped refreshing, SHALL both be
distinguishable from a correct one without reading a clock.

A level with no depth boundary SHALL be reported as no cross-level work at all:
every level of a uniform hierarchy is self-contained, holds no neighbourhood and
walks no rim, so both counts SHALL stay at zero there however the hierarchy is
sculpted. Otherwise "not zero" could not be read as "the region rim was asked
for", which is the whole use of the pair.

Releasing a neighbourhood, releasing a level's cache and rebuilding either SHALL
remain correct and SHALL err toward re-deriving: a level rebuilt from cold
produces the same positions, and re-reading them costs a walk rather than a wrong
answer.

#### Scenario: An interior dab does not walk the region rim
- **WHEN** a stroke is taken entirely inside a refined region, so no dab writes any level but the bound one
- **THEN** the bound level's cross-level neighbourhood is asked for on every dab and re-derived on none of them
- **AND** the outside positions it reports are the ones a hierarchy carrying the same detail and nothing cached would build

#### Scenario: A stroke on the level below re-derives the rim
- **WHEN** a stroke is taken at the level below a refined region, reaching vertices its outside positions are subdivided from
- **THEN** the neighbourhood is re-derived, its topology is unchanged, and its outside positions are the ones a hierarchy carrying the same detail and nothing cached would build

#### Scenario: A uniform hierarchy reports no cross-level work
- **WHEN** a stroke is taken on a hierarchy whose levels all store every patch, at the bound level and at the level below it
- **THEN** both counts stay at zero, while the same stroke on a hierarchy with a depth boundary counts asks

#### Scenario: A stroke on the cage re-derives the rim above it
- **WHEN** a stroke is taken at level 0, where the brush writes the level's mesh directly and the hierarchy reads those positions into the cage rather than writing them back
- **THEN** level 1's cross-level neighbourhood is re-derived, and its outside positions are the ones a hierarchy carrying the same detail and nothing cached would build
