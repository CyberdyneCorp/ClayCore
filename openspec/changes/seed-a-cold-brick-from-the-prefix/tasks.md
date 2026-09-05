## 1. The cache learns what a second consumer needs

- [x] 1.1 `align_to_lattice` on the policy, part of the cache KEY so the two
      consumers never receive each other's entry
- [x] 1.2 A structural witness, so the O(prefix roots) validity digest is
      recomputed only when something structural moved
- [x] 1.3 `find_usable`: the best cached boundary rather than the policy's
      current one, so a stroke keeps hitting as it stamps

## 2. The refill

- [x] 2.1 `clay_brick_cache_eval_requests_seeded`, with the cache CALLER-owned
- [x] 2.2 `clay_sdf_prefix_cache_build_for_refill`
- [x] 2.3 The seed is sampled into the caller's buffer, resolved once per batch
- [x] 2.4 The result is stored as an ordinary seed, so the second touch is warm

## 3. Gates

- [x] 3.1 A seeded refill is the walk's answer, in-band, against a fresh document
- [x] 3.2 The prefix actually served — a cache covering nothing is correct and
      useless, and the two are indistinguishable without this
- [x] 3.3 A null cache is byte-identical to the plain refill
- [x] 3.4 An uncovered window takes the walk rather than a bad seed
- [x] 3.5 A stroke keeps hitting after its boundary has moved
- [x] 3.6 Version lines together; `check_*` green

## 4. What is NOT closed

- [ ] 4.1 A cold window is still linear in history — 0.47 ms at 2,000 items
      against 6.80 at 50,000. Neither the digest (memoised) nor the suffix
      (invariant from 4 to 256 roots) accounts for it. **Attribute the residual
      before designing anything further**; #306 stays open on that.
