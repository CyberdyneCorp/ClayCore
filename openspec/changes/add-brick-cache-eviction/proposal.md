# Proposal: the memory ceiling can be hit but never backed away from

## Why

The brick cache has a budget and no way to give memory back. The whole API is
`memory_usage()`, `dirty_count()` and `tracked_count()` — three getters — and
`submit()` returning `BudgetExceeded`. There is no evict, no trim, no clear, at
`brick::BrickCache` or across the C ABI.

So the budget is a wall, not a ceiling. What happens on the far side of it is
that a submitted brick is **refused**:

```cpp
enum class SubmitResult { Accepted, Stale, BudgetExceeded };
```

The artist keeps sculpting, the dab dirties its 24 bricks, the evaluation runs,
and the results are dropped. The surface stops updating exactly where they are
working, and the only recourse the host has is to destroy the cache and rebuild
it from nothing — which is the most expensive operation available, taken at the
moment the device is least able to afford it.

Two things make this specifically an iPad problem rather than a tidiness one.

**iOS asks for memory back and then takes it.** A memory warning is an
invitation to release; ignoring it is how an app gets killed. The host is
supposed to respond by freeing caches, and this cache — likely its largest
single allocation — has no way to be asked. `memory_usage()` lets the host watch
the number climb and do nothing about it.

**The budget does not cover everything that grows.** The header says so
outright: `memory_usage()` bounds only the fp16 payloads, so the per-key
bookkeeping "grows with how much space has ever been marked dirty, OUTSIDE the
memory budget". A long session over a large model grows a map the budget never
sees. That is honest, documented, and still unbounded.

Nothing here is a design mistake. The cache was specified with "error code /
eviction policy" as alternatives and shipped the error code, which was the right
first half. This is the second half.

## What changes

**Eviction.** The cache can drop surface bricks it is holding and reclaim their
payload, choosing what to drop by a stated policy rather than arbitrarily. A
dropped brick is not lost information: it is a brick that has to be re-evaluated
if it is looked at again, which is exactly what the dirty/request/submit cycle
already does. Eviction is therefore expressible as "mark it as never-evaluated",
which is a state the cache already has.

**Trim to a target.** A host can ask the cache to get down to N bytes and be
told what it managed, so a memory warning has an answer.

**The bookkeeping is bounded too**, or its growth is made visible as a number a
host can act on. Untracking keys with no payload and no dirt is the obvious
route.

**A budget-exceeded submit becomes recoverable rather than terminal**: the host
can free space and resubmit, and the contract for what happened to the refused
brick is stated rather than implied.

All of it reachable from the C ABI, since a packaged host has nothing else.

## What it is not

**Not an automatic eviction loop.** The cache does not get a policy that runs
behind the host's back, for the same reason it publishes no refill loop, thread
pool or time budget: the consumer owns scheduling. Eviction is something a host
*asks for* — on a memory warning, on a view change, on its own timer.

**Not an LRU by default without measurement.** "Least recently used" is the
reflex answer and may be the wrong one here: the bricks a sculptor comes back to
are the ones near where they are working, which is a spatial question, not a
temporal one. Distance from the last edit or from the view frustum may beat
recency. The policy is a decision to make and record, and the host may need to
supply the ordering because only it knows where the camera is.

**Not a change to what a brick contains** or to the dirty/generation protocol. A
brick that is evicted and re-evaluated must produce bit-identical data, which is
the test.

## Open questions

- **What ordering decides what goes.** Recency, distance from the last dirty
  region, distance from a host-supplied point, or a host-supplied key list. The
  last is the most honest given the cache cannot see the camera, and the least
  convenient. To be decided in `design.md`.
- **Whether mips are evicted with their parents.** A level-1 mip covers eight
  full-res bricks and is cheaper to keep than to rebuild; dropping the parents
  while keeping the mip may be the right degradation, and it changes what
  `build_mip` has to assume.
- **What a refused submit costs.** Today the evaluation is done and thrown away.
  Whether the cache should be askable *before* the evaluation — "would this fit"
  — is worth deciding, since the wasted work is the expensive part, not the
  refusal.
- **Whether the budget should count the bookkeeping.** Making
  `memory_usage()` total is more honest and changes the meaning of an existing
  configured number, which is a compatibility question rather than a technical
  one.

## Impact

`brick-cache`'s memory requirement gains the second half it was specified with;
`c-abi` gains the surface. No brick data changes; a host that never evicts sees
today's behaviour exactly.
