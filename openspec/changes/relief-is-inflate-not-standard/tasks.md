## 1. Pin the frame

- [ ] 1.1 Add `relief: each point moves along its own normal, which is Inflate`
      to `tests/unit/test_relief.cpp`: a fin 0.1 thick under one relief stamp
      at `k = rounding = radius = 0.15` gains `k` of half-thickness below its
      top (within 2e-3) and `k` of height, and a point `v + k*w*n(v)` on each
      face lies on the displaced surface.
- [ ] 1.2 In the same test, the shared-direction reference `v + k*w*N` on the
      fin's faces lies at least `0.5k` from the relief surface — the assertion
      that separates the two frames.
- [ ] 1.3 Mutate before trusting it: run the 1.1 thickness assertion against
      the same fin under `move_surface(c, k*N, reach, smoothstep)` (the draw
      frame) and confirm it fails; record the numbers.

## 2. Correct the mapping

- [ ] 2.1 `bindings/c/clay.h`: the `CLAY_OP_RELIEF` comment names Inflate, and
      a paragraph beside it says Standard is an approximation, what decides the
      error (normal spread under the stamp), the fin and smooth-surface numbers,
      and that `clay_layer_move_surface` with a smoothstep ease is the exact
      draw frame for ONE stamp and a stroke of them costs one warp per item per
      dab. Comment only: `CLAY_ABI_*` does not move.
- [ ] 2.2 `include/clay/kernel/tape.h` and `include/clay/scene/types.h`: the
      same correction on `ccombine_relief` / `Op::Relief`.
- [ ] 2.3 `docs/07-brushes-and-features.md`: §9 Standard and Inflate rows, the
      relief section's consequences list, and the Draw row of the
      one-representation table.
- [ ] 2.4 `docs/09-brush-latency-and-coverage.md` inventory and
      `docs/sculpt_comparison.md`: Standard marked as approximated by
      `Op::Relief`, not matched.
- [ ] 2.5 `examples/25_relief.py` docstring: Inflate, with Standard as the
      approximation.

## 3. Verify

- [ ] 3.1 `npx -y @fission-ai/openspec@1.12.0 validate --all --strict`.
- [ ] 3.2 Build cpu-only with tests and run `'-sf=*test_relief.cpp'`, then the
      full suite.
- [ ] 3.3 `python3 tools/check_c_abi.py` (header touched) and
      `python3 tools/release_check.py --skip-slow`.
- [ ] 3.4 PR body: the measurement tables, the three refuted spellings, no ABI
      transition.
