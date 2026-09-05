# c-abi — a refill that can seed a cold brick

Delta for `seed-a-cold-brick-from-the-prefix`.

## ADDED Requirements

### Requirement: A refill may be given a prefix to seed cold bricks from

A brick that has been refilled before carries a seed and evaluates only what the
document gained since. A brick that has NOT SHALL be able to start from a cached
prefix of its layer's history rather than from nothing, evaluating only the roots
after that prefix.

The prefix cache SHALL be the CALLER'S, not the document's, on the same terms as
every other cache in this ABI: a document holding a pointer to memory the host
can free is a hazard this ABI refuses.

Passing no cache SHALL produce byte-identical results at byte-identical cost to
the refill that takes none, so a host that has not opted in cannot be affected.

The seeded result SHALL be stored as an ordinary seed, so a second touch of the
same window takes the existing warm path rather than this one.

A caller SHALL be able to learn how many bricks the prefix actually served. A
cache that covers nothing produces correct output and no acceleration, and
without a count those are indistinguishable — which is the whole failure mode of
this feature.

The prefix SHALL NOT be built by the refill. A build is a bake, and paying for
one inside a refill puts the cost back on the frame the feature exists to
protect.

#### Scenario: A seeded refill is the walk's answer
- **WHEN** a window with no seed is refilled with a prefix cache, and the same window is refilled on a fresh document with none
- **THEN** the two agree within the band to the sampling tolerance the prefix declares

#### Scenario: Not opting in costs nothing
- **WHEN** the seeded refill is called with no cache
- **THEN** its results are byte-identical to the refill that takes no cache, and no brick is reported as seeded

#### Scenario: The count says whether it worked
- **WHEN** a refill is given a cache that covers the requested windows
- **THEN** it reports how many bricks the prefix served, and that count is greater than zero

### Requirement: A prefix is built for the lattice its consumer reads

A cached prefix SHALL record which lattice it was built for, and a consumer SHALL
receive only a prefix built for its own.

A seed read on the lattice it was built for is the stored sample; one read half a
cell away is an interpolation of two, which is a different field by about a
quarter of a cell. The two consumers this ABI has read different lattices — a
smoothing transaction reads the layer's own region, a brick refill reads a grid
anchored at the world origin — so one prefix cannot serve both and SHALL NOT be
offered to both.

#### Scenario: Each consumer gets its own
- **WHEN** a prefix is built for a refill and another for a smoothing transaction, at the same resolution
- **THEN** they are separate entries, and neither is returned to the other's consumer

#### Scenario: A refill's seed is exact
- **WHEN** a refill is seeded from a prefix built for refills
- **THEN** the seeded values agree with the full walk to floating-point rounding rather than to a fraction of a cell
