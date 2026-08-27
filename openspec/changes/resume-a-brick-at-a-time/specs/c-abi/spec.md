# c-abi

## MODIFIED Requirements

### Requirement: a brick refill resumes per brick

Refilling a brick SHALL evaluate only what the document gained since that
brick's seed was taken, whenever that can be proven exact for THAT BRICK. The
decision SHALL NOT depend on whether the other bricks of the same call can be
resumed, nor on their agreeing about a revision.

A brick that cannot be served — no seed, a lattice the seed does not match, a
cull pad that has moved, a prefix that produced no accumulator, or a document
change that is not an append — SHALL take the full walk, and SHALL do so without
costing any other brick in the call its resume.

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

### Requirement: a resume follows the layer the appends went to

A refill SHALL resume only when the appends recorded since the seed was taken
were made to the layer whose chain the suffix would extend. Appends to any other
layer SHALL make the plan unusable, and the refill SHALL take the full walk.

NodeIds are per-layer: every layer's content numbers its nodes from 1, so
comparing an appended id against another layer's roots can agree by coincidence.
The layer SHALL be compared, not inferred from the ids.

#### Scenario: an append to a layer beneath, whose id collides

- **GIVEN** a document with two visible SDF layers
- **AND** the layer beneath has one fewer root than the active one, so its next
      id equals the active layer's last root
- **AND** a refill has stored seeds for a row of bricks
- **WHEN** an item is appended to the layer BENEATH and the row is refilled
- **THEN** no brick is resumed
- **AND** the values equal a document holding the same items and never resumed

## ADDED Requirements

### Requirement: the resumable path is observable

The C ABI SHALL report, per document, how many bricks refills have answered from
a seed and how many have taken the full walk, together with the seed store's
occupancy, its byte cost and the budget it is evicted against, through a
versioned descriptor.

The two paths are bit-identical by contract, so no output of a refill can
distinguish them; without these counts a fast path that stops firing is
indistinguishable from one that works.

#### Scenario: reading the counts

- **WHEN** a host calls `clay_document_resume_stats` with `struct_size` set
- **THEN** it receives cumulative `resumed_bricks` and `refilled_bricks` counts
- **AND** the counts never reset, so an interval is read as their difference

#### Scenario: a seed is a performance cache only

- **WHEN** every stored seed is dropped
- **THEN** later refills produce the same geometry, taking longer to do it
