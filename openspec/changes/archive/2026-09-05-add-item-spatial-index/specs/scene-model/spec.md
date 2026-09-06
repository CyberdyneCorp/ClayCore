# scene-model — culling asks an index, not every item

Delta for `add-item-spatial-index`.

## MODIFIED Requirements

### Requirement: Per-brick tape culling
For brick evaluation the compiler SHALL emit per-brick tapes containing only the items whose influence bound intersects that brick (the Dreams design), preserving evaluation semantics exactly.

Deciding which items those are SHALL NOT require visiting every item in the document. The compiler SHALL consult a spatial index over item influence bounds, so that the cost of culling one brick scales with the number of items NEAR that brick rather than with the size of the document.

The index SHALL be derived from the same definition of reach the compiler already uses — `item_influence_bound`, and `item_influence_is_local` for whether an item has a bound at all — so that no second notion of what an item touches can go stale against the first. An item that is not local SHALL be emitted unconditionally rather than placed in the index.

The index SHALL be owned by, and invalidated with, the compiled tape it culls for, so that a document mutation cannot leave the index and the tape disagreeing about the same document.

#### Scenario: Culled tape matches full tape
- **WHEN** a brick is evaluated with its culled tape and with the full scene tape
- **THEN** the brick data is bit-identical, and the culled tape length is ≤ the full tape length

#### Scenario: Culling cost does not follow document size
- **WHEN** the same brick is culled from a document of 100 items and from a document of 10 000 items with the same local density
- **THEN** the time to produce the culled tape does not grow in proportion to the item count

#### Scenario: A non-local item is never culled away
- **WHEN** a document contains an item whose influence is unbounded and a brick that its geometry does not come near
- **THEN** that item is present in the brick's culled tape, and the brick data is bit-identical to the full-tape result

#### Scenario: An edit is visible to the next cull
- **WHEN** an item is added, moved or removed and a brick is culled immediately afterwards
- **THEN** the culled tape reflects the edit, exactly as a full recompile would
