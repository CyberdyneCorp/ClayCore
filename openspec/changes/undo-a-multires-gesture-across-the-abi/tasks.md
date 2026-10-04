## 1. The gap, measured on origin/main (64c5ecfe)

- [x] 1.1 `clay.h` names `MultiresDelta` and `SculptLayerDelta` only to say they
      do not cross the ABI; pyclay does not expose either
- [x] 1.2 The C++ records refuse only by counts: a twin hierarchy and a
      relevelled one both accept and are written (8 assertions with the binding
      disabled)
- [x] 1.3 `apply_to_multires` records only the base half; with an active pass a
      recorded stroke is empty (7 cases / 31 assertions with the layer half
      nulled)
- [x] 1.4 A reverted pass leaves `clay_multires_dirty_blocks` empty

## 2. Tests first

- [x] 2.1 `tests/unit/test_c_multires_delta.cpp`: round trips through the plain
      sculptor (stroke and stamp), level 0, a refined region's rim, a mirrored
      pair, a sculpt-layer transaction, and the plain sculptor on an active pass;
      the same stroke again after an undo; twin / relevel / removed-pass
      refusals; coalescing; the byte formula and spill; dirty patches after a
      replay; empty, cleared and size-query cases; `clay_multires_sculpt_layer_stroke_commit_into` on a non-empty
      record
- [x] 2.2 The file does not compile on main (the API does not exist); each
      engine decision is proven by mutation (1.2, 1.3, 1.4, and continuing a
      record into a different pass: 2 assertions)

## 3. Engine

- [x] 3.1 `MultiresDelta` / `SculptLayerDelta`: `matches`, split counts,
      `encoded_size`; `revert` / `apply` reuse `matches`
- [x] 3.2 `mesh::MultiresGesture`: both halves, binding, replay, levels,
      envelope encode / decode
- [x] 3.3 `brush::apply_to_multires` takes a trailing, defaulted
      `SculptLayerDelta*`

## 4. C ABI

- [x] 4.1 `clay_multires_delta` and its nine calls; `clay_multires_delta_stats`
      with `struct_size`
- [x] 4.2 `clay_multires_sculptor_stamp_recorded`, `_apply_stroke_recorded`,
      `clay_multires_sculpt_layer_stroke_commit_into`; the shipped calls share
      one implementation with a null record
- [x] 4.3 The two "does not cross this ABI" paragraphs replaced
- [x] 4.4 ABI 0.124.0 -> 0.125.0 in `CMakeLists.txt`, `bindings/c/clay.h`,
      `pyproject.toml`

## 5. Docs and gates

- [x] 5.1 `docs/05-claycore-library.md` and `docs/07-brushes-and-features.md`
      § 8d
- [x] 5.2 Unit suite, `check_c_abi.py`, `check_binding_parity.py`,
      `release_check.py --skip-slow`, `openspec validate --strict`
