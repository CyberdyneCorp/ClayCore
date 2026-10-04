## MODIFIED Requirements

### Requirement: Per-brick tape culling
For brick evaluation the compiler SHALL emit per-brick tapes containing only the items whose influence bound intersects that brick (the Dreams design), preserving evaluation semantics exactly.

Deciding which items those are SHALL NOT require visiting every item in the document. The compiler SHALL consult a spatial index over item influence bounds, so that the cost of culling one brick scales with the number of items NEAR that brick rather than with the size of the document.

The index SHALL be derived from the same definition of reach the compiler already uses — `item_influence_bound`, and `item_influence_is_local` for whether an item has a bound at all — so that no second notion of what an item touches can go stale against the first. An item that is not local SHALL be emitted unconditionally rather than placed in the index.

The index SHALL be owned by, and invalidated with, the compiled tape it culls for, so that a document mutation cannot leave the index and the tape disagreeing about the same document.

A SQUASHED placement — a non-uniform per-axis scale on the item, on its layer, or both — emits a field that can be short of the true distance by up to `q = max(s) / min(s)`, the two levels' ratios multiplied. The cull SHALL widen such an item's bound by `(q - 1) * (band + pad + w)` before testing it, where `band` is how far inside the cull region the brick's samples lie, `pad` is the compiler's own cull pad, and `w` is the dilation the bound already carries (rounding and combine support). A group SHALL be widened by its subtree's widest such term plus `(q - 1)` times its own blend support. The widened item SHALL still be culled from any brick its field cannot reach; a squashed placement SHALL NOT be exempted from the cull. The index and any coarse plan SHALL widen identically, so a planned compile stays byte-identical to an unplanned one. A plan made for a narrower band than a region carries SHALL NOT be used for that region where the document holds a squashed placement. A document with no per-axis scale SHALL make the same cull decisions whatever band a region carries.

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

#### Scenario: A squashed placement is kept where its field reaches the band
- **WHEN** an item scaled (4, 1, 1), or an item on a layer scaled (4, 1, 1), sits more than band + pad from a brick by its bound, but its field inside the brick is within the band
- **THEN** the brick's refill matches `clay_eval_points` at every in-band sample, with the batch planned for the requests' band

#### Scenario: A planned compile widens as the walk does
- **WHEN** a document with squashed items, squashed groups and a squashed layer is culled per brick with and without the cull index and a plan made for the region's band
- **THEN** the tapes are byte-identical, and a plan made for no band is refused for a region that carries one

#### Scenario: An unsquashed document ignores the band
- **WHEN** a document with no per-axis scale is culled with a region that carries a band and with the same region that carries none
- **THEN** the two tapes are byte-identical
