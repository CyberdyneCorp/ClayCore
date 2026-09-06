# c-abi

## MODIFIED Requirements

### Requirement: a brick refill resumes per brick

Refilling a brick SHALL evaluate only what the document gained since that
brick's seed was taken, whenever that can be proven exact for THAT BRICK. The
decision SHALL NOT depend on whether the other bricks of the same call can be
resumed, nor on their agreeing about a revision.

A stored seed SHALL be identified by everything that decides what it describes:
the brick coordinate, the lattice it was sampled on (dims and voxel size) and
the BAND it was culled under. A request that differs in any of them SHALL NOT
find that seed, neither to serve from nor to overwrite.

A brick that cannot be served — no seed, a lattice or band the seed does not
match, a cull pad that has moved, a prefix that produced no accumulator, or a
document change that is not an append — SHALL take the full walk, and SHALL do
so without costing any other brick in the call its resume.

#### Scenario: a window that moves

- **GIVEN** a refill has stored seeds for a row of bricks
- **AND** one item is appended to the active layer
- **WHEN** a refill asks for that row shifted by one brick
- **THEN** the bricks the row still covers are answered from their seeds
- **AND** only the newly entered brick takes the full walk

#### Scenario: one brick without a seed

- **GIVEN** a refill has stored seeds for a set of bricks
- **AND** one item is appended to the active layer
- **WHEN** a refill asks for those bricks plus one never asked for before
- **THEN** the seeded bricks are still answered from their seeds

#### Scenario: bricks stamped by different dabs

- **GIVEN** two sets of bricks whose seeds were stored at different revisions
- **AND** the document has only been appended to since the older of them
- **WHEN** a refill asks for both sets together
- **THEN** each brick is carried forward from its own revision
- **AND** every one of them is answered from its seed

#### Scenario: what it resumes to is what a full walk would say

- **GIVEN** bricks resumed across several appends from several revisions
- **WHEN** their values are compared with a document built with the same items
      and never resumed
- **THEN** they are bit-identical, not equal within a tolerance

## ADDED Requirements

### Requirement: a seed serves only the band it was taken under

A brick's tape is culled against the brick dilated by the request's band, so a
narrower band drops items a wider one keeps. A refill SHALL NOT serve a request
from a seed taken under a different band, in either direction, and SHALL take
the full walk instead.

The values a differing band changes are not confined to distances the band
would clamp: measured on a dim-8, 0.05 cache, a seed taken at a 0.15 band and
served to a 0.6-band request was wrong at 9 of 512 samples, worst 0.105 — two
voxels — at a true distance of 0.354, well inside the band asked for and so a
sample a submit stores rather than clamps.

#### Scenario: a wider band than the seed was taken under

- **GIVEN** a document holding an item outside a narrow band's cull region and
      inside a wider one's
- **AND** a refill has stored a seed for a brick at the narrow band
- **AND** one item is appended to the active layer
- **WHEN** that brick is refilled at the wider band
- **THEN** it is not resumed
- **AND** its values are bit-identical to a document holding the same items and
      never resumed

#### Scenario: a narrower band than the seed was taken under

- **GIVEN** a refill has stored a seed for a brick at a wide band
- **AND** one item is appended to the active layer
- **WHEN** that brick is refilled at a narrower band
- **THEN** it is not resumed
- **AND** its values are those of the narrower band's own culled tape

#### Scenario: the same band still resumes

- **GIVEN** a refill has stored a seed for a brick
- **AND** one item is appended to the active layer
- **WHEN** that brick is refilled at the band its seed was taken under
- **THEN** it is answered from its seed

### Requirement: two caches over one document keep their own seeds

A brick coordinate is unique only within a lattice, so caches of different dims
or voxel sizes over one document name the same coordinate. Their seeds SHALL be
held separately: a refill by one SHALL NOT evict or overwrite what the other
stored, and both SHALL go on resuming while a stroke asks them in turn.

#### Scenario: a coarse and a fine cache alternating

- **GIVEN** a coarse cache and a fine cache covering one brick coordinate over
      one document
- **AND** both have been refilled once, so both hold a seed
- **WHEN** an item is appended and both are refilled, repeatedly
- **THEN** the seed store holds an entry for each
- **AND** every refill after the first is answered from a seed
- **AND** each cache's values are those of a document that never resumed
