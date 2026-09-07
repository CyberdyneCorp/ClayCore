# Tasks

- [x] Add `item_geometry_reach_in_document` to `src/scene/bounds.cpp`: the box a
      change confined to one item's own geometry reaches in the document, with
      the chain pad, the ancestor group supports and the layer folds — and a
      refusal for everything the local argument does not cover.
- [x] Add `command_surface_delta_bound` to `src/scene/commands.cpp`: the one
      supported edit-delta, a `SetTransformCmd` on a visible Intersect item.
- [x] Leave `item_nonlocality`, `item_influence_bound`,
      `node_influence_bound_in_document`, `clay_layer_node_influence_bound` and
      `clay_brick_cache_mark_dirty_nodes` untouched.
- [x] Use the delta in `apply_edit` (bindings/c/clay_c.cpp) when BOTH sides
      claim one, and keep the conservative union otherwise.
- [x] Add `clay_layer_set_transform_bound`, ABI 0.89.0 — the edit plus the
      region it changed, for a host that keeps a brick cache.
- [x] The mathematical probe, over the fixture matrix:
      `tests/unit/test_intersect_delta_bound.cpp`.
- [x] The probe's own test: a one-sided bound must FAIL it
      ("intersect delta: the probe has teeth").
- [x] The oracle: incremental refill against a full rebuild, brick payloads and
      meshes compared: `tests/unit/test_intersect_delta_oracle.cpp`.
- [x] The acceptance gate, in COUNTS: the dirty-brick count must not follow the
      layer's extent
      ("intersect oracle: the refill does not scale with the layer's extent").
- [x] `BM_OperandDragIntersect`, `BM_OperandDragIntersectLayerBound` and
      `BM_OperandDragSubtract` at the reference and ten-times extents, reporting
      bound, refill and remesh time beside the dirty-brick count and the AABB
      volume ratio. NO threshold added to `tools/check_bench.py`.
- [x] Bump CMakeLists.txt, pyproject.toml and CLAY_ABI_* to 0.89.0.
- [x] Document the entry point in `bindings/c/clay.h` and `docs/05-claycore-library.md`.
