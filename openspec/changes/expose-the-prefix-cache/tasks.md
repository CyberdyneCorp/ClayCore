## 1. The handle (17.3)

- [x] 1.1 `clay_sdf_prefix_cache`, opaque, host-created and host-destroyed,
      taking no document and destroying with void — the `clay_brick_cache` rule,
      adopted rather than re-decided
- [x] 1.2 `clay_sdf_prefix_cache_create` takes the byte budget; 0 means OFF
- [x] 1.3 `_set_max_bytes`, `_invalidate_layer`, `_clear`
- [x] 1.4 `_stats` into a `struct_size` descriptor, including the seeded/fallback
      counters — the only way a test can tell "fast because the cache worked"
      from "fast because the window was touched twice"

## 2. The policy, mirroring the C++ nesting

- [x] 2.1 `clay_sculpt_policy` grows `min_history_roots`,
      `keep_live_suffix_roots` and `prefix_max_bytes`; sampling stays the one
      `cell_size`/`band`/`padding` it already has — see design.md §2
- [x] 2.2 `read_sculpt_policy` fills `SdfSculptPolicy::prefix` from them
- [x] 2.3 The old layout still reads correctly through `struct_size`, and a host
      that sets none of the three gets today's behaviour exactly

## 3. Building and asking

- [x] 3.1 `clay_sdf_prefix_cache_build(cache, doc, layer, policy, token)` — the
      door `SdfSourceField::open` deliberately is not
- [x] 3.2 `clay_sdf_prefix_boundary_for(doc, layer, policy, out_roots)` so a host
      can ask whether a layer is worth caching without paying for one
- [x] 3.3 Cancellable, and a cancelled build caches nothing

## 4. The consumer

- [x] 4.1 `clay_sdf_smooth_begin_cached(doc, layer, policy, cache, token)`
- [x] 4.2 `clay_sdf_smooth_begin` keeps its exact behaviour, null cache included
- [x] 4.3 Beginning with a cache that holds nothing BUILDS NOTHING

## 5. Bindings and gates

- [x] 5.1 pyclay parity; `check_binding_parity.py` green.
      C-ONLY, and registered in `C_ONLY_FOLLOW_UPS` with the reason rather than
      shipped quietly: the cache accelerates ONE thing, the SDF Smooth
      transaction, and pyclay does not expose that transaction at all. A Python
      handle for it would be a handle with nothing to accelerate, which reads as
      coverage and is not. The order is `clay_sdf_smooth_*` in pyclay first and
      the cache with it
- [x] 5.2 `check_c_abi.py` green, including the grown descriptor.
      It caught one thing: `clay_sdf_prefix_cache_invalidate_layer` said "layer"
      without saying which of the three the library means. Registered in
      `DOCUMENT_LAYER_CALLS` beside `clay_brick_cache_mark_dirty_layer`, which is
      the same reading for the same reason
- [x] 5.3 ABI 0.78.0 -> 0.79.0 in `CMakeLists.txt`, `bindings/c/clay.h`,
      `pyproject.toml` — the three that have shipped out of step twice
- [x] 5.4 `tests/c_api/` — lifetime (cache outlives the document), the budget,
      the boundary query, and a cold window served from a built prefix
- [x] 5.5 Proven to catch its regression: with the cache argument dropped on the
      floor the cold-window test must fail, and the revert must COMPILE.
      DONE, and it took two attempts to make the revert compile at all, which is
      the point of requiring that: dropping the argument outright is
      `-Werror=unused-parameter`, so the probe assigns the cache to a local and
      then nulls it. The revert compiles with 0 errors and
      `served.seeded_windows` reads 0 against a required > 0

## 6. The scheduling note (17.2)

- [x] 6.1 `docs/09` gains the build/dab/break-even table and the two facts the
      pair gives: the build follows the history, the volume does not
- [x] 6.2 `docs/05` §11 gains the host recipe — ask the boundary, build between
      gestures, begin cached, read the counters
- [x] 6.3 Tick `add-sdf-prefix-cache` 17.2 and 17.3 with what closed them

## 7. Left open

- [ ] 7.1 17.4 STAYS OPEN with a sharper reason than it had: a cached prefix
      stores one scalar per sample and a boundary inside a group leaves a STACK
      of pending combines. `TapeCheckpointFrame` exists because the brick-resume
      seed IS a stack; a volume cannot hold one
