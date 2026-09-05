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

## 4. The residual, attributed and removed

- [x] 4.1 Attributed by phase timing rather than by inference, after two wrong
      guesses: 3.5 ms of a 5.4 ms window sat in the suffix COMPILE.
      `compile_layer_suffix` given a cull region but NO index re-derives the
      document's cull pad by walking every layer, per brick. The cold path had
      been written to skip the index because `plan_frontier` does not need one
      — but the compile underneath it does.
- [x] 4.2 Take the cull index on the prefix path: released phase 3,500 us ->
      24 us, and a cold window goes flat.
- [x] 4.3 `BM_ColdWindowSeeded` vs `BM_ColdWindowSeededSmall` holds the
      flatness; the ratio against `BM_ColdWindowPlain` says what it is worth.
