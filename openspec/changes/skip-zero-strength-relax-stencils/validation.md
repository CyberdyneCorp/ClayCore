# Zero-strength Smooth/Relax priming validation

## Reproduction and attribution

Host e99ace5b, unchanged Core v0.113.0 (260b7797), Release CPU fields,
RTX 5060 Vulkan renderer, i9-12900K. The running application's MCP `measure`
operation reproduced the pointer-down stall on an isolated default sphere.
Temporary timing around transaction begin, zero-strength update, preview absorb,
and rest composition showed under 1 ms for transaction begin, 187–197 ms for
zero-strength update, 5–6 ms for absorb, and no material rest cost on this fixture.
The temporary instrumentation is not part of the implementation.

## Alternating live comparison for the stencil shortcut at 69d07a54

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

## Combined application validation

Host e99ace5b was built against Core 69d07a54 in Release with CPU fields and
RTX 5060/Vulkan rendering. All 12 tests passed without adapter skips: native
MCP E2E (3), sculpt latency (4), settlement (3), and rendered brushes (2).
This checks all three Core optimizations together with the companion host fixes.
The host's committed v0.113.0 engine pin was restored after the experiment.

## Additional all-brush action check

A subsequent comparison used the complete combined builds: host e99ace5b with
Core `477f1b23` versus Core `69d07a54`, three alternating pairs across all 13 tools.
All 78 tool/run cases completed successfully. Tracked uploaded-byte counts
matched in all 117 paired begin/continue/end comparisons. These counts verify
consistent submitted workload, not byte equality of the uploaded contents.

Heavy concurrent analysis jobs from another project slowed both applications
substantially during this run. Its timings are not evidence of a speedup or
regression and do not replace the controlled priming comparison above.
A retry after those jobs ended encountered a new set of concurrent Rust builds; per-run process/load snapshots confirmed the renewed contention. Both
attempts completed all 78 cases, but neither is used as a latency gate. The
rendered assertions are covered separately by the 12 integration tests.

## Allocation follow-up

A finer phase probe found that zero-strength relaxation still copied and
rewrote its input after the stencil shortcut. The follow-up avoids snapshots
for zero strength and reports the whole-volume selection without rewriting
identical values. Full initial materialization reserves the known final sample
payload once. Local/subsequent materialization keeps amortized vector growth.

Two regression tests use the existing allocation counter and fail on 69d07a54:

| Regression fixture | Allocated bytes before | Allocated bytes fixed |
| --- | ---: | ---: |
| Three whole-volume zero-strength passes at the band floor | 702,840 | 0 |
| Full initial source materialization (1,000,188-byte sample payload) | 3,991,440 | 2,008,560 |

These are cumulative allocation counts inside the tested calls, not whole-app
resident-memory measurements. Reports and samples are checked alongside them.
Both allocation gates pass ASan/UBSan with leak detection. All 11 CPU CTest
targets pass again (97.71 seconds), including 2,756 C++ cases and 756 Python
tests with one skip. The 128-case nonzero-output comparison remains byte-identical
with the original main implementation after separating the atomic pass helper.

Cognitive complexity: atomic pass 22, orchestration 7, materialization 30 (main:
28). A local phase prototype also reduced copying time, but ran under machine
contention; the allocation regressions are the deterministic performance evidence.
Full sanitizer and combined-host verification for the allocation follow-up are
still pending; the earlier live timings and combined tests concern 69d07a54.

## Remaining validation

Platform CI must cover this new change; the earlier green Core revision
477f1b23 predates it. Issue #531 remains open for its broader latency goal and
field degradation at depth.
