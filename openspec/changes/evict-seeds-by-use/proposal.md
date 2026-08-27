# Proposal: evict a brick's seed by USE, and keep the order describing the store

## Why

The seed store is bounded at 64 MB and evicted through a second structure — a
deque of keys — that the store itself was not keeping in step with. Two things
about it were wrong, and both come from the same place.

**`touch_region` erased entries and left their keys.** The eviction order was
maintained in exactly three places and the region invalidation was not one of
them. Its nodes are not counted by the byte accounting, so they accumulated
OUTSIDE the 64 MB budget for as long as a session ran non-append edits. Worse,
a key erased there and stored again later took the fresh branch a second time,
so one brick occupied several slots and the order stopped describing the store
it was ordering.

**Eviction was FIFO by first insertion, never refreshed on use.** The key was
pushed only when the map reported it fresh, so re-storing a brick did not move
it. For a stroke that is exactly backwards: the hot working set is stored at the
first dab and rewritten by every dab after it, so it sits nearest the front and
a store under pressure evicts precisely the bricks the next dab is about to ask
for — while ground the brush crossed once and left is kept.

Nothing fails today at 64 MB for a dim-8 distance-only cache: measured through
`clay_document_resume_stats`, such a brick is 2,048 B, so that is 32,768 of them
and few sessions reach it. A dim-16 cache with colour is 64 KiB a brick,
or about 1,000, which a large model does reach — and the failure mode there is a
stroke evicting the bricks it is about to ask for again, turning the resumable
refill back into the full walk it exists to avoid.

## What

One structure change carries both fixes. The order becomes a `std::list` and
every entry holds its own node, so the two operations that were expensive or
absent are each O(1) and neither is a search:

- **Removing an entry takes its node with it.** `touch_region` erases from the
  list through the entry's own iterator, so the invariant "one node per live
  seed, and no others" holds — which makes the order's size the entry count, and
  a leak or a duplicate observable as those two diverging.
- **Using an entry moves it to the back.** Refreshed wherever a seed is used:
  handed out by `seed_for`, rewritten by `store_active` (the resumed path's
  store, and the strongest statement there is that a brick is in the working
  set), and re-stored by `store_seed`.

Eviction then takes the front, and the "never the brick just written" special
case goes away with it: with more than one entry the front is never the most
recently used.

The byte accounting sums vector CAPACITY rather than size, so the budget bounds
what the store has ALLOCATED. `store_active` clears the colour buffer for a
request that carries none, and a cleared vector keeps its buffer, so a
size-based sum reported memory the entry still held as free. The alternative,
`shrink_to_fit` on every clear, would free and reallocate one lattice per brick
per dab on the path this cache exists to keep cheap.

Two things the ceiling does not bound, both stated in the spec rather than left
to be discovered. Eviction keeps the most recently used value whatever the
budget says — a budget below one brick would otherwise discard what the caller
just stored, which is the brick the next dab reads — so a budget of 0 reports
one entry and its bytes, not zero. And `store_active` is the one store that does
not evict, so a stroke adding colour to entries that had none can carry the
store above its budget until the next full-path store trims it; that is
pre-existing and unchanged here.

The budget becomes a member so a test can lower it. It, and the order's size,
are reachable only through `bindings/c/clay_internal.h`, which is not installed
and carries no version guarantee.

## Not a hang

The duplicated keys suggest an eviction loop that cannot make progress. It could
not be constructed: reaching the byte ceiling needs many live entries, which is
exactly the state in which the order is not down to a couple of duplicates. This
is reported and fixed as unbounded growth and a corrupted order, not as a hang.

## Impact

No ABI change, no version bump: `clay_resume_stats` keeps its layout, and
`budget` now reports the store's live budget instead of the compile-time
constant — the same number for every host, since only a test moves it.

Bounded work per operation, all of it O(1): one list splice where there was
nothing, one list erase where a key was leaked.

## Non-goals

**A host-settable seed budget.** A host does not size this store — it is a pure
performance cache whose contents change no geometry, and a host that wants the
memory back has `clay_brick_cache_trim` for the cache that does hold geometry.
Publishing a knob would mean promising the store's shape across versions. The
test hook stays internal until a host asks for it with a reason.

**Reporting the order's size to hosts.** A store whose order size differed from
its entry count would be describing its own bug, not a state to react to.
