# scene-model — the per-brick cull reads what the index cached

Delta for `reject-a-brick-without-the-node`.

## ADDED Requirements

### Requirement: A per-brick cull decides from the cached entry, not from the node
Where a coarse plan supplies a chain's survivors, the per-brick cull SHALL decide whether a survivor reaches this brick from the cached entry alone — its locality flag and its bound — and SHALL NOT reach the node behind it to do so. Both values were computed when the chain was cached and cannot have changed while the index is valid, and the node is reached through a pointer into the layer's node map, so re-deriving them costs a cache miss per rejected item: about 12,000 of them per brick, over 24 bricks, on a 50,000-item document.

The decision SHALL remain the compiler's own — a survivor reaches the brick unless it is local, finite and misses the region — and SHALL cover groups and items alike, a group's cached entry being local with an infinite bound exactly where its subtree is non-local.

An entry a plan supplies SHALL be visible, so the per-brick cull need not check: a chain caches only the children it would compile.

#### Scenario: A planned per-brick tape is the tape the plain compile gives
- **WHEN** each brick of a batch is compiled through an index and a plan, and again with a plain cull region and no index
- **THEN** the tapes are byte-identical, including for chains holding groups, non-local items and infinite bounds

#### Scenario: A plan handed in without a cull region is dropped
- **WHEN** a document is compiled with an index and a plan but no cull region
- **THEN** the plan is ignored and the tape is the whole document's, since a plan without a region could only mean a pruned whole-document tape

#### Scenario: The saving is in the rejects
- **WHEN** a dab's bricks are compiled over a batch survivor list far larger than any one brick keeps
- **THEN** what a rejected survivor costs is the cached test alone
