## 1. Pin the frame

- [x] 1.1 Add `relief: each point moves along its own normal, which is Inflate`
      to `tests/unit/test_relief.cpp`: a fin 0.1 thick under one relief stamp
      at `k = rounding = radius = 0.15` gains `k` of half-thickness at
      y = 0.4, where the weight is full (within 2e-3) and `k` of height, and a point `v + k*w*n(v)` on each
      face lies on the displaced surface.
- [x] 1.2 In the same test, the shared-direction reference `v + k*w*N` on the
      fin's faces lies at least `0.5k` from the relief surface — the assertion
      that separates the two frames.
- [x] 1.3 Mutate before trusting it: run the 1.1 thickness assertion against
      the same fin under `move_surface(c, k*N, reach, smoothstep)` (the draw
      frame) and confirm it fails; record the numbers. Review probe, same
      fin: relief half-thickness at y = 0.4 goes 0.05 -> 0.20 and the top
      0.50 -> 0.65; `move_surface` leaves the half-thickness at 0.05 and
      lifts the top to 0.6225, so the assertion can fail.
      Recorded: with the test's tape swapped for the fin under
      `brush::move_brush(c, (0, 0.15, 0), radius 0.45, ease_smoothstep)`, the
      thickness check fails `0.15 < 0.002` (half-thickness 0.05, top 0.622647),
      the top check fails by 0.0274, every partial- and full-weight face point
      of 1.1 sits 0.029-0.15 off the surface, and 1.2's shared-direction
      reference lands ON it (0). Relief reads 0.2 and 0.65. The draw-frame
      numbers stay pinned as `relief: the draw frame, spelled as move_surface,
      leaves the fin's faces put`, so the discriminator cannot go inert.

## 2. Correct the mapping

- [x] 2.1 `bindings/c/clay.h`: the `CLAY_OP_RELIEF` comment names Inflate, and
      a paragraph beside it says Standard is an approximation, what decides the
      error (normal spread under the stamp), the fin and smooth-surface numbers,
      and that `clay_layer_move_surface` with a smoothstep ease is the exact
      draw frame for ONE stamp and a stroke of them costs one warp per item per
      dab. Comment only: `CLAY_ABI_*` does not move.
- [x] 2.2 `include/clay/kernel/tape.h` and `include/clay/scene/types.h`: the
      same correction on `ccombine_relief` / `Op::Relief`.
- [x] 2.3 `docs/07-brushes-and-features.md`: §9 Standard and Inflate rows, the
      relief section's consequences list, and the Draw row of the
      one-representation table.
- [x] 2.4 `docs/09-brush-latency-and-coverage.md` inventory and
      `docs/sculpt_comparison.md`: Standard marked as approximated by
      `Op::Relief`, not matched.
- [x] 2.5 `examples/25_relief.py` docstring: Inflate, with Standard as the
      approximation.

## 3. Verify

- [x] 3.1 `npx -y @fission-ai/openspec@1.12.0 validate --all --strict`.
- [x] 3.2 Build cpu-only with tests and run `'-sf=*test_relief.cpp'`, then the
      full suite.
- [x] 3.3 `python3 tools/check_c_abi.py` (header touched) and
      `python3 tools/release_check.py --skip-slow`.
      c-abi OK against the cpu-only build's shared library (hygiene + FFI);
      `clay.h`, `kernel/tape.h` and `scene/types.h` compare identical to
      origin/main with comments stripped (`gcc -fpreprocessed -dD -E -P`), so
      `CLAY_ABI_*` stays at 0.117.0. release_check: every row passes except
      `dialect` (no Metal Toolchain on this machine) and `device` (stale on
      main too), plus the four manual hardware rows, which pass on origin/main
      and fail here because ANY byte in `include/clay/kernel/` expires them —
      the `tape.h` comment does. Comment-only, so a waiver at release names it.
- [x] 3.4 PR body: the measurement tables, the three refuted spellings, no ABI
      transition.
- [x] 3.5 Review. Mutated the kernel itself (`ctape_combine_dist`), not only
      the test's tape: dropping relief's smoothstep fails 6 own-normal points
      (0.0065-0.0086 against 1e-3); scaling its amplitude to 0.95k fails the
      thickness and top checks (0.0075 against 2e-3). Corrected: the
      `move_surface` stamp is the draw frame's direction but not relief's
      profile (it rises 0.82k at this mapping, 0.1226 on the fin); docs/09 and
      sculpt_comparison now carry the frame caveat on Crease/DamStandard and
      ClayBuildup; the spec scenario excludes corners (0.093k measured there);
      the 30-dab step scale is read back at full precision (5.2151e-6); and
      3.3 no longer names a build artifact, which the task-symbols gate cannot
      resolve in a clean tree.
