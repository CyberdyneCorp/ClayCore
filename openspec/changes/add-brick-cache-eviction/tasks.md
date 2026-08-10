# Tasks: add-brick-cache-eviction

- [ ] 1.1 DECIDE and record in `design.md`: what ordering decides which bricks are dropped. Recency is the reflex answer and probably wrong — a sculptor returns to what is near their last edit, which is spatial, not temporal. Consider host-supplied ordering, since the cache cannot see the camera
- [ ] 1.2 DECIDE and record: whether a level-1 mip is dropped with its parents or kept as a degraded stand-in, and what `build_mip` may assume afterwards
- [ ] 1.3 DECIDE and record: whether `memory_usage()` starts counting the per-key bookkeeping. It is more honest and it changes the meaning of an already-configured number
- [ ] 1.4 Eviction on `brick::BrickCache`: drop a brick's payload, return it to the never-evaluated state, reclaim `surface_bytes_`
- [ ] 1.5 Trim to a target, reporting the usage reached and the count dropped — including the case where the target cannot be met
- [ ] 1.6 Bound or expose the per-key bookkeeping: untrack keys with no payload and no dirt, or make the growth queryable. The header already admits this grows outside the budget; leaving it unbounded and invisible is not an outcome
- [ ] 1.7 A refused submit is recoverable: state what happens to the refused brick, and make free-then-resubmit work under the ordinary generation rule
- [ ] 1.8 Consider and decide: an "would this fit" query before evaluation, since the wasted evaluation is the expensive part of a refusal, not the refusal
- [ ] 1.9 C ABI: trim, the statistics that expose both kinds of growth, and any host-supplied ordering. No loop, no timer, no threshold
- [ ] 1.10 Test: evict, re-dirty, re-evaluate, resubmit from an unchanged document — bit-identical to the pre-eviction data
- [ ] 1.11 Test: trim with dirty bricks and outstanding requests in flight; no submitted brick lands in a slot it does not own, and stale requests are still refused as stale
- [ ] 1.12 Test: a cache that is never trimmed behaves exactly as it does today, including at the budget wall
- [ ] 1.13 Test the long-session shape: dirty a large region repeatedly, confirm the tracked-key growth is now bounded or reported
- [ ] 1.14 Document the memory-warning flow in `docs/05-claycore-library.md` — the sequence a host runs on a platform memory warning, end to end
