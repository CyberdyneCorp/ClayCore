## ADDED Requirements

### Requirement: A dirtied brick is refilled against the current document
A brick the host marks dirty SHALL be refilled to the value the CURRENT document gives it, whatever sequence of edits preceded the mark and whether or not a refill ran between them. A stored seed SHALL be carried forward across an edit only when neither that edit nor any earlier edit the seed has not yet absorbed can reach the brick's cull region. A seed that cannot be shown current SHALL NOT answer a refill as if it were. The next refill of that brick SHALL walk it in full.

Carrying a seed forward is a performance cache and SHALL NOT be removed where it is sound: after appends that did not reach a brick, a later region edit that does not reach it either SHALL leave the brick answerable from its seed.

#### Scenario: Append an intersect, move it, refill
- **WHEN** an intersect item is appended to a layer and then moved with a uniform transform, with no refill between, and the item's influence bound is then marked dirty and refilled
- **THEN** the cache holds exactly the bricks, bit for bit, that a document built with the item at its final place holds

#### Scenario: A seed no append reached is still resumed
- **WHEN** a filled cache sees an append on one side of the model and a region edit elsewhere, and a region that neither reached is then re-dirtied and refilled
- **THEN** bricks of that region are answered from their seeds, and the result is bit-identical to a document built without the edit history
