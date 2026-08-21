# Tasks: add-brick-cache-eviction

- [x] 1.1 DECIDE: SPATIAL, furthest-from-a-host-supplied-focus first — and MEASURED rather
      than argued. On a walking-stroke fixture that trims to 60% after every dab:
      spatial evicted 405 bricks with 85 re-requested by the next dab (21% thrash);
      arbitrary evicted 647 with 341 re-requested (53%). Spatial is 2.5x better on thrash
      AND reached the same target having evicted 40% fewer bricks, because it was not
      dropping things that came straight back and had to be dropped again. The focus is a
      parameter because only the host knows where the camera points; one point rather than
      an ordering, because a memory warning wants an answer now.
- [x] 1.2 DECIDE: the mip is KEPT. Dirtying a child invalidates it because the shape
      changed; eviction does not change the shape, it drops a cached copy of it. Keeping the
      coarse stand-in is the whole value of a memory-warning response — the silhouette
      survives at an eighth of the memory and a host that built level 1 keeps something to
      draw. `build_mip` may assume nothing new: it already requires all children clean, and
      evicted children are not evaluated, so it simply declines as it always did.
- [x] 1.3 DECIDE: NO — a separate `bookkeeping_bytes()` instead. `memory_usage()` is
      compared against a budget hosts have already configured; making it grow by a term it
      never included would change the meaning of a number in the field rather than report a
      new fact. Two pools, two numbers, and `clay_brick_stats` grows a field by its
      `struct_size` rule.
- [x] 1.4 Eviction on `brick::BrickCache`: `evict(key)`, returning it to never-evaluated
- [x] 1.5 `trim_to(bytes, focus)` and `trim_to(bytes)`, reporting the count dropped; the
      target may be unreachable because dirty bricks are never dropped, and `memory_usage()`
      says where it got to
- [x] 1.6 BOTH: `bookkeeping_bytes()` reports it and `forget_empty()` reclaims it.
      — The rule is NARROWER than "no payload", and the first implementation got it wrong:
      a key is forgettable only when an UNTRACKED key answers identically. Never-evaluated
      and evaluated-OUTSIDE qualify, because a missing brick reads as +band. An INSIDE brick
      does NOT — it holds no lattice but carries real information, and forgetting one would
      report solid interior as empty. Pinned by a test.
- [x] 1.7 Recoverable: trim, then re-dirty and resubmit. The generation is deliberately NOT
      reset by eviction, so a request already in flight is still refused as stale — eviction
      is not a licence to accept work scheduled against a different revision. And a key
      dropped by `forget_empty` answers Stale on a late submit, so the work is discarded
      rather than landing in a slot it no longer owns.
- [x] 1.8 Consider and decide: an "would this fit" query before evaluation.
      — DECIDED: a NUMBER, not a predicate. `clay_brick_stats.brick_bytes` reports what one
      surface brick costs, so a host answers `memory_usage + pending * brick_bytes <=
      memory_budget` with arithmetic it owns. A `would_it_fit()` predicate is true only
      until the next submit, which in a threaded host is immediately, so it would imply a
      guarantee the cache cannot make; the arithmetic is honest about being a snapshot. The
      point stands either way — the expensive part of a refusal is the evaluation already
      spent, so a host that checks first trims BEFORE evaluating rather than after.
- [x] 1.9 C ABI: `clay_brick_cache_trim` (NULL focus = no preference, a statement rather
      than an omission), `_evict`, `_forget_empty`, and `bookkeeping_bytes` on
      `clay_brick_stats`. No loop, no timer, no threshold.
- [x] 1.10 Test: evict, re-dirty, refill — bit-identical decoded lattice, and the byte count returns to exactly what it was
- [x] 1.11 Test: a trim to 0 from the far side leaves every queued brick queued, and the queued work still completes and lands
- [x] 1.12 Test: trimming to an already-met target changes nothing, and the budget wall still refuses rather than silently evicting
- [x] 1.13 Test: a large dirtied region around a small sphere tracks far more than it stores; `forget_empty` reclaims it and leaves the payloads untouched
- [x] 1.14 Document the memory-warning flow in `docs/05-claycore-library.md` — the sequence a host runs on a platform memory warning, end to end, with the three properties it can rely on and the measured reason the policy is spatial
