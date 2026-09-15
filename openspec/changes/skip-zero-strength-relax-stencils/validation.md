# Zero-strength Smooth/Relax priming validation

## Reproduction and attribution

Host e99ace5b, unchanged Core v0.113.0 (260b7797), Release CPU fields,
RTX 5060 Vulkan renderer, i9-12900K. The running application's MCP `measure`
operation reproduced the pointer-down stall on an isolated default sphere.
Temporary timing around transaction begin, zero-strength update, preview absorb,
and rest composition showed under 1 ms for transaction begin, 187–197 ms for
zero-strength update, 5–6 ms for absorb, and no material rest cost on this fixture.
The temporary instrumentation is not part of the implementation.

## Final alternating live comparison

Three alternating before/fixed pairs, fresh isolated application state per run,
Smooth, Relax and Standard; undo to history depth zero between tools. Radius 1
sphere, brush radius 0.18, strength 0.65, X symmetry, voxel 0.02, brick dimension
8, band 3. Measure stroke begin at [0,0,1], continue at [0.12,0,1], then end.
The two builds differ only by this relax implementation; both use the host's
existing v0.113.0 engine otherwise and identical phase instrumentation. No build
or test from this task ran concurrently with the final comparison.

All 18 tool/run cases completed. The priming update itself fell from 177.089
to 28.853 ms median across the six calls in each version. Median measured
pointer-down latency:

| Tool | Before ms | Fixed ms |
| --- | ---: | ---: |
| Smooth | 257.000 | 110.008 |
| Relax | 240.254 | 97.334 |
| Standard control | 1.219 | 1.203 |

Release medians remain approximately unchanged: Smooth 88.660 / 89.882 ms,
Relax 89.460 / 91.749 ms, Standard 71.644 / 70.773 ms. Release tracked upload
counts are unchanged. This change removes unused priming arithmetic; it does
not remove source materialization or the host's complete-preview meshing.
Neither these timings nor the control prove every brush meets 16 ms.

## Correctness

- The new zero-strength regression fails against prior production code: masks
  are sampled unnecessarily and a stored negative zero can become positive zero.
- The fixed regression covers zero, negative zero and clamped-negative strength,
  whole-field and regional work, three passes, zero mask calls, bit-identical
  serialized samples with existing band adjustment, and pre-pass cancellation.
- A transaction regression proves priming still materializes the entire lattice,
  exports all preview bricks, leaves the document unchanged, avoids duplicate
  deltas on a repeated no-op, and supports subsequent nonzero dabs.
- All 11 CPU CTest targets pass (92.56 seconds), including Python and the full
  C++ suite. Targeted relax, sculpt and cancellation tests: 63 cases and
  2,095,823 assertions, also passing ASan/UBSan with leak detection.
- An independent nonzero-output comparison links the original relax object from
  main 6beebdd5 and the final object separately against the same library. All
  128 combinations produce byte-identical serialized volumes: strengths
  0.1/0.8/1/2, radii 1/2, iterations 1/3, whole-field/regional, no mask and
  masks 0/0.5/1. Both concatenated outputs have SHA-256
  `b5a8ea8d0efe8eca5b96c3d3202b78e95ce99c1f0ccf9609324c6993848687fc`.
- Clang-tidy cognitive complexity with real include paths: relaxation orchestration
  35 on main to 32, extracted stencil average 4. Systems target: 35.
- Layering and binding parity pass; strict OpenSpec validation passes all 64 items.
- GCC 13's array-bounds warning in FieldVolume's copying constructor was reproduced
  by compiling the original main relax source with the same flags. No new warning
  exemption was added.

## Reproduce core checks

```sh
cmake --build --preset cpu-only --target clay_unit_tests pyclay clay_c_smoke clay_cli -j 6
ctest --preset cpu-only --output-on-failure
cmake --build --preset asan-ubsan --target clay_unit_tests -j 4
ASAN_OPTIONS=detect_leaks=1 build/asan-ubsan/tests/clay_unit_tests \
  --source-file='*test_relax.cpp,*test_sdf_sculpt.cpp,*test_cancellation.cpp'
openspec validate --all --strict
```

This machine requires `LD_PRELOAD=/usr/lib/x86_64-linux-gnu/libstdc++.so.6`
when running Python/CTest with its Conda runtime.

## Remaining validation

Final combined host rendered tests and platform CI must cover this new change;
the earlier green Core revision 477f1b23 predates it. Issue #531 remains open for
its broader latency goal and field degradation at depth.
