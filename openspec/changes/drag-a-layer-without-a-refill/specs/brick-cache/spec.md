# brick-cache — a refill for one layer, or for everything but it

Delta for `drag-a-layer-without-a-refill`.

## ADDED Requirements

### Requirement: A refill can be scoped to a layer or to its complement
The batched brick evaluation SHALL accept a layer scope: the whole document (what it does today and the default), the document with one named layer excluded, or that named layer alone.

A scoped refill SHALL produce the same lattice, the same band clamping and the same brick classification rules as the unscoped one — it evaluates a different field, not a different way. Its results SHALL be storable and readable by the same calls, at the same strides.

A scoped result SHALL NOT be used as a seed for a refill at any other scope. A seed is the value of a culled tape and two scopes compile different tapes, so a scoped result served as an unscoped seed is a partial field answered as a whole one — wrong, with nothing in the result to indicate it.

A scoped refill SHALL therefore STORE NO SEED at all, rather than storing one under a scope-aware key. Storing nothing is the stronger of the two: it cannot be defeated by a key that forgets a dimension, and the cost is only that a scoped refill is always a full walk, which is what a preview drawn once per gesture wants anyway.

#### Scenario: A scoped refill matches an unscoped one on a one-layer document
- **WHEN** a document holding one visible SDF layer is refilled unscoped, and then scoped to that layer alone
- **THEN** the two results are bit-identical

#### Scenario: A scope change does not resume from the wrong seed
- **WHEN** the same bricks are refilled scoped to a layer, then refilled unscoped with no edit between
- **THEN** the unscoped results equal a cold unscoped refill's

#### Scenario: The excluded layer contributes nothing
- **WHEN** bricks are refilled with a layer excluded, and that layer is then edited
- **THEN** refilling with it excluded again returns the values it returned before
