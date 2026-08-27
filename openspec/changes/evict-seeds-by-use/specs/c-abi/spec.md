# c-abi — the seed store evicts by use

Delta for `evict-seeds-by-use`.

## ADDED Requirements

### Requirement: The seed store's eviction order describes the store
The structure that orders kept brick values for eviction SHALL hold exactly one
entry per stored value: none for a value that has been discarded, and never more
than one for the same brick.

Discarding a value — by eviction, or by an edit whose region reaches that
brick — SHALL remove its place in the order at the same time. Storing a brick
that already has a value SHALL NOT add a second place for it.

This matters because the order's own memory is NOT counted against the store's
byte budget: places left behind by a discarded value grow outside the ceiling
the budget describes, without bound, for as long as edits keep arriving.

#### Scenario: Repeated region-invalidating edits leave nothing behind
- **GIVEN** bricks refilled, then an item moved that reaches every one of them, repeatedly
- **WHEN** the store is asked how many places its eviction order holds
- **THEN** the count equals the number of values the store holds, after every cycle

#### Scenario: A brick discarded and stored again occupies one place
- **GIVEN** bricks whose values an edit discarded, and which a later refill stores again
- **THEN** the eviction order holds one place per brick, not one per time it was stored

### Requirement: A kept brick value is evicted by last use, not by first storage
When the store is over its byte budget it SHALL discard the LEAST RECENTLY USED
value first. A value is used when it is read to answer a refill and when it is
written by one; either SHALL make it the most recently used.

Ordering by first storage is not equivalent and is wrong for the access pattern
the store exists to serve: a stroke stores its working set at the first dab and
rewrites it at every dab after, so a first-storage order discards precisely the
bricks the next dab is about to ask for while keeping ground the brush crossed
once and left.

The most recently used value SHALL NOT be discarded, so that a budget smaller
than a single brick does not discard what was just stored.

#### Scenario: The rewritten brick survives and the abandoned one does not
- **GIVEN** a hot brick stored FIRST and a cold brick stored after it, a budget with room for both, and a stroke that rewrites only the hot one
- **WHEN** a third brick is stored and the store goes over budget
- **THEN** the cold brick's value is discarded and the hot brick's is still there to be resumed from

### Requirement: The seed store's byte budget bounds what it has allocated
The bytes the store reports and evicts against SHALL count the memory its
entries HOLD, not the memory they are currently using. A refill that carries no
colour empties an entry's colour buffer without releasing it, so a
usage-based count would report memory the store still holds as free and make the
ceiling optimistic by an amount no host can see.

The ceiling has one carve-out, which is the floor the requirement above places
on eviction: the single most recently used value is kept even when it alone
exceeds the budget. A budget with room for less than one value therefore reports
one entry and that entry's bytes, above the budget, rather than an empty store.

#### Scenario: The reported bytes stay at or under the budget
- **GIVEN** a budget with room for at least one stored value
- **WHEN** more bricks are stored than that budget has room for
- **THEN** the bytes reported by `clay_document_resume_stats` are at or under the budget it reports, and the entry count is what fits

#### Scenario: A budget below one value keeps that value anyway
- **GIVEN** a budget smaller than what one brick's value costs
- **WHEN** bricks are stored
- **THEN** the store holds exactly one entry — the most recently used — and reports its bytes, which are above the budget

### Requirement: The seed store's budget is not part of the C ABI
The store's byte budget SHALL NOT be host-settable, and the size of its eviction
order SHALL NOT be reported to hosts. A kept value is a pure performance cache —
discarding every one of them changes no geometry — and a store whose order size
differed from its value count would be describing its own defect rather than a
state a host could act on.

Reaching either from a test SHALL be through an internal header that is not
installed and carries no version guarantee, so that changing or removing it is
not an ABI break.

#### Scenario: The public descriptor is unchanged
- **WHEN** a host reads `clay_resume_stats`
- **THEN** it finds the same fields at the same offsets as before, with `budget` reporting the budget in force
